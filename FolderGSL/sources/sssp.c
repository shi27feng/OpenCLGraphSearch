#include <sssp.h>
#include <CL/cl.h>
#include <stdio.h>
#include <cl_utils.h>
#include <benchmark_utils.h>
#include <edge_vertice_message.h>
#include <float.h>
#include <unistd.h>
#include <libgen.h>

#define GROUP_NUM 32

static cl_program program;
static cl_context context;
static cl_command_queue command_queue;
static cl_device_id device;

static void build_kernel(size_t device_num)
{
    device = cluInitDevice(device_num,&context,&command_queue);
    //printf("%s\n\n",cluGetDeviceDescription(device,device_num));
    cl_device_type device_type;
    clGetDeviceInfo(device,CL_DEVICE_TYPE,sizeof(cl_device_type),&device_type,NULL);
    int group_num = 1;
    (device_type == CL_DEVICE_TYPE_GPU) ? (group_num = GROUP_NUM) : (group_num = 1);

    char* filename = "/sssp.cl";
    char tmp[1024];
    sprintf(tmp, "-DGROUP_NUM=%d",group_num);
    char cfp[1024];
    char kernel_file[1024];
    sprintf(cfp, "%s",__FILE__);
    sprintf(kernel_file,"%s%s%s",dirname(dirname(cfp)),"/kernels",filename);
    program = cluBuildProgramFromFile(context,device,kernel_file,tmp);
}


void sssp(Graph* graph,unsigned source,cl_float* out_cost,cl_uint* out_path, unsigned device_num, unsigned long *time, unsigned long *precalc_time)
{
    // Build the kernel
    build_kernel(device_num);

    // start time calculation
    unsigned long start_time = time_ms();

    // Determine the type of the device
    cl_device_type device_type;
    clGetDeviceInfo(device,CL_DEVICE_TYPE,sizeof(cl_device_type),&device_type,NULL);

  	//Allocate necessary Data
    cl_uint* sourceVerticesSorted = (cl_uint*)malloc(graph->E * sizeof(cl_uint));
    cl_uint* messageWriteIndex = (cl_uint*) malloc(graph->E * sizeof(cl_uint));
    cl_uint* numEdgesSorted = (cl_uint*) calloc(graph->V, sizeof(cl_uint));
    cl_uint* offset = (cl_uint*) calloc(graph->V,sizeof(cl_uint));
    cl_uint* oldToNew = (cl_uint*) calloc(graph->V,sizeof(cl_uint));
    cl_uint* newToOld = (cl_uint*) calloc(graph->V,sizeof(cl_uint));
    cl_uint messageBuffersize;
    cl_int err;

    //Preprocess Indices and initialize VertexBuffer, Calculate the number of incoming Edges for each vertex and set the sourceVertex of each edge
    // It depends on the Devicetype, whether to make a data-layout-remapping or not. Both calculations are made on GPU as it's normally faster
    if(device_type == CL_DEVICE_TYPE_GPU)
        preprocessing_parallel_gpu(graph,messageWriteIndex,sourceVerticesSorted,numEdgesSorted,oldToNew,newToOld,offset,&messageBuffersize,0);
    else
        preprocessing_parallel_cpu(graph,messageWriteIndex,sourceVerticesSorted,numEdgesSorted,oldToNew,newToOld,offset,&messageBuffersize,0);

    //printf("Time for Preprocessing : %lu\n",time_ms()-start_time);

    // If requested, save the time it took for precalculating the messageBuffer
    if(precalc_time != NULL)
        *precalc_time = time_ms()-start_time;

    // Allocate data for the messageBuffer and fill it with FLT_MAX
    cl_float* messageBuffer = (cl_float*) malloc(messageBuffersize * sizeof(cl_float));
    for(int i = 0; i<messageBuffersize;i++){
        messageBuffer[i] = FLT_MAX;
    }

    // create necessary buffers

    cl_mem edge_buffer = clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(cl_uint) * graph->E, NULL, &err);
    CLU_ERRCHECK(err,"Failed creating edgebuffer");

    cl_mem weight_buffer = clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(cl_float) * graph->E, NULL, &err);
    CLU_ERRCHECK(err,"Failed creating weight_buffer");

    // Create Memory Buffers for Helper Objects
    cl_mem message_buffer= clCreateBuffer(context,CL_MEM_READ_WRITE,sizeof(cl_float) * messageBuffersize, NULL,&err);
    CLU_ERRCHECK(err,"Failed creating message_buffer");

    cl_mem message_buffer_path= clCreateBuffer(context,CL_MEM_WRITE_ONLY,sizeof(cl_uint) * messageBuffersize, NULL,&err);
    CLU_ERRCHECK(err,"Failed creating message_buffer_path");

    cl_mem messageWriteIndex_buffer= clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(cl_uint) * graph->E, NULL,&err);
    CLU_ERRCHECK(err,"Failed creating messageWriteIndex_buffer");

    cl_mem sourceVertices_buffer= clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(cl_uint) * graph->E, NULL,&err);
    CLU_ERRCHECK(err,"Failed creating sourceVertices_buffer");

    cl_mem  numEdges_buffer= clCreateBuffer(context,CL_MEM_READ_ONLY,sizeof(cl_uint) * graph->V, NULL,&err);
    CLU_ERRCHECK(err,"Failed creating numEdges_buffer");

    cl_mem cost_buffer = clCreateBuffer(context,CL_MEM_READ_WRITE,sizeof(cl_float) * (graph->V), NULL, &err);
    CLU_ERRCHECK(err,"Failed creating cost_buffer");

    cl_mem path_buffer = clCreateBuffer(context,CL_MEM_WRITE_ONLY,sizeof(cl_uint) * (graph->V), NULL, &err);
    CLU_ERRCHECK(err,"Failed creating path_buffer");

    cl_mem  active_buffer = clCreateBuffer(context,CL_MEM_READ_WRITE,sizeof(cl_bool) * (graph->V), NULL, &err);
    CLU_ERRCHECK(err,"Failed creating active_buffer");

    cl_mem  offset_buffer = clCreateBuffer(context,CL_MEM_READ_WRITE,sizeof(cl_uint) * (graph->V), NULL, &err);
    CLU_ERRCHECK(err,"Failed creating offset_buffer");

    cl_mem finished_flag = clCreateBuffer(context,CL_MEM_WRITE_ONLY,sizeof(cl_bool), NULL,&err);
    CLU_ERRCHECK(err,"Failed creating finished_flag");

    // Copy Graph Data to their respective memory buffers
    err = clEnqueueWriteBuffer(command_queue, edge_buffer, CL_FALSE, 0, graph->E * sizeof(cl_uint), graph->edges , 0, NULL, NULL);
    err = clEnqueueWriteBuffer(command_queue, weight_buffer, CL_FALSE,0, graph->E * sizeof(cl_uint), graph->weight,0, NULL, NULL);

    err = clEnqueueWriteBuffer(command_queue, message_buffer, CL_FALSE, 0, messageBuffersize * sizeof(cl_float), messageBuffer , 0, NULL, NULL);
    err = clEnqueueWriteBuffer(command_queue, messageWriteIndex_buffer, CL_FALSE, 0, graph->E * sizeof(cl_uint), messageWriteIndex , 0, NULL, NULL);
    err = clEnqueueWriteBuffer(command_queue, sourceVertices_buffer, CL_FALSE, 0, graph->E * sizeof(cl_uint), sourceVerticesSorted , 0, NULL, NULL);

    err = clEnqueueWriteBuffer(command_queue, numEdges_buffer, CL_FALSE, 0, graph->V * sizeof(cl_uint), numEdgesSorted , 0, NULL, NULL);
    err = clEnqueueWriteBuffer(command_queue, offset_buffer, CL_TRUE, 0, graph->V * sizeof(cl_uint), offset , 0, NULL, NULL);

    CLU_ERRCHECK(err,"Failed copying graph data to buffers");

    // Create the Kernels
    cl_kernel init_kernel = clCreateKernel(program,"initialize",&err);
    CLU_ERRCHECK(err,"Failed to create initializing kernel from program");

    cl_kernel edge_kernel = clCreateKernel(program,"edgeCompute",&err);
    CLU_ERRCHECK(err,"Failed to create EdgeCompute kernel from program");

    cl_kernel vertex_kernel = clCreateKernel(program,"vertexCompute",&err);
    CLU_ERRCHECK(err,"Failed to create vertexCompute kernel from program");

    //Set KernelArguments
    cluSetKernelArguments(init_kernel,4,sizeof(cl_mem),(void*)&cost_buffer,sizeof(cl_mem),(void*)&path_buffer,sizeof(cl_mem),(void*)&active_buffer, sizeof(unsigned), &oldToNew[source]);
    cluSetKernelArguments(edge_kernel,8,sizeof(cl_mem),(void*)&edge_buffer,sizeof(cl_mem),(void*)&sourceVertices_buffer,sizeof(cl_mem),(void*)&messageWriteIndex_buffer,sizeof(cl_mem),(void*)&message_buffer,sizeof(cl_mem),(void*)&message_buffer_path,sizeof(cl_mem),(void*)&cost_buffer,sizeof(cl_mem),(void*)&weight_buffer,sizeof(cl_mem),(void*)&active_buffer,sizeof(cl_mem));
    cluSetKernelArguments(vertex_kernel,8,sizeof(cl_mem),(void*)&offset_buffer,sizeof(cl_mem),(void*)&message_buffer,sizeof(cl_mem),(void*)&message_buffer_path,sizeof(cl_mem),(void*)&numEdges_buffer,sizeof(cl_mem),(void*)&cost_buffer,sizeof(cl_mem),(void*)&path_buffer,sizeof(cl_mem),(void*)&active_buffer,sizeof(cl_mem),(void*)&finished_flag);

    // Execute the OpenCL kernel for initializing Buffers
    size_t globalEdgeSize = graph->E;
    size_t globalVertexSize = graph->V;
    CLU_ERRCHECK(clEnqueueNDRangeKernel(command_queue, init_kernel, 1, NULL, &globalVertexSize, NULL, 0, NULL, NULL),"Error executing InitializeKernel");

    // start looping both main kernels
    bool finished;

    cl_event edge_kernel_event;
    cl_event vertex_kernel_event;
    cl_ulong time_start_edge, time_end_edge;
    cl_ulong time_start_vertex, time_end_vertex;
    long unsigned total_time_edge = 0;
    long unsigned  total_time_vertex = 0;

    while(true)
    {
        CLU_ERRCHECK(clEnqueueNDRangeKernel(command_queue, edge_kernel, 1, NULL, &globalEdgeSize, NULL, 0, NULL, &edge_kernel_event), "Failed to enqueue Dijkstra1 kernel");
        CLU_ERRCHECK(clEnqueueNDRangeKernel(command_queue, vertex_kernel, 1, NULL, &globalVertexSize, NULL, 0, NULL, &vertex_kernel_event), "Failed to enqueue Dijkstra2 kernel");
        err = clEnqueueReadBuffer(command_queue,finished_flag,CL_TRUE,0,sizeof(cl_bool),&finished,0,NULL,NULL);

        clGetEventProfilingInfo(edge_kernel_event, CL_PROFILING_COMMAND_START, sizeof(time_start_edge), &time_start_edge, NULL);
        clGetEventProfilingInfo(edge_kernel_event, CL_PROFILING_COMMAND_END, sizeof(time_end_edge), &time_end_edge, NULL);

        clGetEventProfilingInfo(vertex_kernel_event, CL_PROFILING_COMMAND_START, sizeof(time_start_vertex), &time_start_vertex, NULL);
        clGetEventProfilingInfo(vertex_kernel_event, CL_PROFILING_COMMAND_END, sizeof(time_end_vertex), &time_end_vertex, NULL);

        total_time_edge += time_end_edge - time_start_edge;
        total_time_vertex += time_end_vertex - time_start_vertex;


        if(finished)
            break;

        finished = true;
        err = clEnqueueWriteBuffer(command_queue, finished_flag, CL_TRUE, 0, sizeof(cl_bool), &finished , 0, NULL, NULL);

    }
    printf("Function: kernel edge\t%lu ms\n",total_time_edge/1000000);
    printf("Function: kernel vertex\t%lu ms\n",total_time_vertex/1000000);
    //printf("Time for Calculating edgeVerticeMessage : %lu\n",total_time);

    // Read back the data of the cost and path buffer
    cl_float* cost_parallel = (cl_float*) malloc(sizeof(cl_float) * graph->V);
    cl_uint* path_parallel = (cl_uint*) malloc(sizeof(cl_uint) * graph->V);
    err = clEnqueueReadBuffer(command_queue,cost_buffer,CL_FALSE,0,sizeof(cl_float) * graph->V,cost_parallel,0,NULL,NULL);
    err = clEnqueueReadBuffer(command_queue,path_buffer,CL_TRUE,0,sizeof(cl_uint) * graph->V,path_parallel,0,NULL,NULL);

    // Put the results in the right order
    unsigned tmp;
    for(int i = 0; i<graph->V;i++)
    {
        out_cost[i] = cost_parallel[oldToNew[i]];
        if((tmp = path_parallel[oldToNew[i]]) != CL_UINT_MAX)
            out_path[i] = newToOld[path_parallel[oldToNew[i]]];
        else
            out_path[i] = CL_UINT_MAX;
    }

    //Free Allocated Data
    free(sourceVerticesSorted);
    free(messageBuffer);
    free(messageWriteIndex);
    free(offset);
    free(oldToNew);

    free(cost_parallel);
    free(path_parallel);

    // Finalize OpenCl Stuff
    err = clFlush(command_queue);
    err |= clFinish(command_queue);
    err |= clReleaseKernel(init_kernel);
    err |= clReleaseKernel(edge_kernel);
    err |= clReleaseKernel(vertex_kernel);
    err |= clReleaseProgram(program);
    err |= clReleaseMemObject(edge_buffer);
    err |= clReleaseMemObject(weight_buffer);
    err |= clReleaseMemObject(message_buffer);
    err |= clReleaseMemObject(message_buffer_path);
    err |= clReleaseMemObject(messageWriteIndex_buffer);
    err |= clReleaseMemObject(numEdges_buffer);
    err |= clReleaseMemObject(sourceVertices_buffer);
    err |= clReleaseMemObject(cost_buffer);
    err |= clReleaseMemObject(path_buffer);
    err |= clReleaseMemObject(active_buffer);
    err |= clReleaseMemObject(offset_buffer);
    err |= clReleaseMemObject(finished_flag);
    err |= clReleaseCommandQueue(command_queue);
    err |= clReleaseContext(context);

    CLU_ERRCHECK(err, "Failed during finalizing OpenCL in function sssp()");

    // If requested, save the time for calculating sssp
    if(time != NULL)
        *time = time_ms()-start_time;
}
