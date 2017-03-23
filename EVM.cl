void memcpy_SIMD (int W_OFF, int cnt, __local unsigned* dest, __global unsigned* src)
{
    for(int IDX = W_OFF; IDX<cnt; IDX+=GROUP_NUM)
    {
        dest[IDX] = src[IDX];
    }

    barrier(CLK_LOCAL_MEM_FENCE);

}

__kernel void calculateWriteIndices(__global unsigned *edges, __global unsigned *oldToNew, __global unsigned *offset, __global volatile unsigned *helper, __global unsigned *messageWriteIndex)
{
    size_t id = get_global_id(0);
    unsigned old_source = edges[id];
    unsigned new_source = oldToNew[old_source];
    unsigned group_id = new_source/GROUP_NUM;
    unsigned inner_id = new_source%GROUP_NUM;
    messageWriteIndex[id] = offset[group_id] + inner_id + (atomic_inc(&helper[old_source])*GROUP_NUM);

}

__kernel void sort_source_vertex(__global unsigned *sourceVertices, __global unsigned *oldToNew, __global unsigned* sorted)
{
    size_t id = get_global_id(0);
    unsigned source = sourceVertices[id];
    sorted[id] = oldToNew[source];
}

__kernel void preprocess(__global unsigned *vertices,__global unsigned *edges,__global unsigned *sourceVertices, __global volatile unsigned *numEdges)
{
    size_t id = get_global_id(0);
    unsigned offset = vertices[id];
    unsigned num_neighbors = vertices[id+1] - offset;

    //offset + i belongs to the unique edge id
    for(int i = 0; i<num_neighbors;i++)
    {
        unsigned dest = edges[offset+i];
        atomic_inc(&numEdges[dest]);
        sourceVertices[offset+i] = id;
    }

}

__kernel void maxima(__global unsigned *numEdges, global unsigned *maxima)
{
    size_t id = get_global_id(0);
    size_t local_id = get_local_id(0);
    size_t group_id = id/GROUP_NUM;

    __local unsigned sizes[GROUP_NUM];

    // copy my work to local
    memcpy_SIMD(local_id,GROUP_NUM,sizes,&numEdges[group_id*GROUP_NUM]);

    if(local_id == 0)
    {
        unsigned max = 0;

        for(int i = 0; i<GROUP_NUM;i++)
            if(sizes[i] > max)
                max = sizes[i];

        maxima[group_id] = max * GROUP_NUM;
    }

}
//Theoretically you could use another bool array to check wheter MessageBuffer was changed for current Vertex
float combine(__global float *messageBuffer, unsigned numMessages, unsigned index)
{
    float min = FLT_MAX;
    for(int i = 0; i<numMessages;i++)
    {
        unsigned new_index = index + (i*GROUP_NUM);
        if(min>messageBuffer[new_index])
            min = messageBuffer[new_index];
    }
    return min;
}

__kernel void initialize(__global float *cost, __global bool *active, unsigned source)
{
    size_t id = get_global_id(0);
    if(id == source)
    {
        cost[id] = 0.0f;
        active[id] = true;
    }
    else
    {
        cost[id] = FLT_MAX;
        active[id] = false;
    }
}


__kernel void edgeCompute(__global unsigned *edges, __global unsigned *sourceVertex, __global unsigned *messageWriteIndex, __global float *messageBuffer,__global float *cost,__global float *weight, __global bool *active)
{
    size_t id = get_global_id(0);
    unsigned source = sourceVertex[id];
    // can this be achieved coalescing?
    if(active[source])
    {
        messageBuffer[messageWriteIndex[id]] = cost[source] + weight[id];
    }
}

__kernel void vertexCompute(__global unsigned *offset, __global float *messageBuffer, __global unsigned *numEdges,__global float *cost, __global bool *active, global bool* finished)
{
    size_t id = get_global_id(0);
    unsigned numMessages = numEdges[id];
    unsigned index = offset[id/GROUP_NUM];
    index += id%GROUP_NUM;
    float msg_min = combine(messageBuffer,numMessages, index);
    if(cost[id] > msg_min)
    {
        cost[id] = msg_min;
        active[id] = true;
        *finished = false;
    }
    else
        active[id] = false;
}

__kernel void assign_bucket(__global unsigned *input, unsigned max, unsigned min, __global unsigned *offset,unsigned num_buckets,__global volatile unsigned *bucket_count,__global unsigned *bucket_index)
{
    size_t id = get_global_id(0);

    unsigned value = input[id];
    unsigned bucket_id = ((value-min)*(num_buckets-1))/(max-min);
    bucket_index[id] = bucket_id;

    offset[id] = atomic_inc(&bucket_count[bucket_id]);
}

__kernel void appr_sort(__global unsigned *key, __global unsigned *key_sorted, __global unsigned *offset, __global unsigned* bucket_count, __global unsigned *bucket_index, __global unsigned *oldToNew)
{
    size_t id = get_global_id(0);

    unsigned k = key[id];
    unsigned b_index = bucket_index[id];
    unsigned count = bucket_count[b_index];
    unsigned off = offset[id];
    off = off+count;
    key_sorted[off] = k;
    oldToNew[id] = off;
}

