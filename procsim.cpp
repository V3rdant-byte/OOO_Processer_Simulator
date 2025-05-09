#include "procsim.hpp"
#include <cstdio>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <queue>
#include <bits/stdc++.h>
using namespace std;
/*Processor related global variables*/
proc_t processor;
std::vector<proc_inst_t> dispatch_queue;
std::vector<proc_rs_entry_t> scheduling_queue;
std::vector<bool> valid_register;
vector<proc_rob_entry_t> ROB;
uint64_t gen_tag;
int execute_count[3];
std::queue<proc_rs_entry_t> result_bus_shifter;

/*Debug related global variables*/
std::vector<vector<int>> debug_table(6, vector<int>(100000));
std::vector<int> instruction_counter(5, 1);
int cycles_counter;
uint64_t max_dispatch_queue_size = 0;
uint64_t num_dispatch_queue = 0;
uint64_t average_dispatch_queue_size = 0;
uint64_t num_fired_instructions;
int debug_cycle = 155;

/**
 * Subroutine for initializing the processor. You many add and initialize any global or heap
 * variables as needed.
 * XXX: You're responsible for completing this routine
 *
 * @r ROB size
 * @k0 Number of k0 FUs
 * @k1 Number of k1 FUs
 * @k2 Number of k2 FUs
 * @f Number of instructions to fetch
 */
void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f) 
{
    /*allocate memory for the processor*/
    memset(&processor, 0, sizeof(proc_t));
    processor.cdb_size = r;
    processor.k0_size = k0;
    processor.k1_size = k1;
    processor.k2_size = k2;
    processor.fetch_width = f;

    /*Initialize the scheduling queue and valid registers*/
    for (uint64_t i = 0; i < 2 * (processor.k0_size + processor.k1_size + processor.k2_size); i++)
    {
        proc_rs_entry_t scheduling_queue_entry;
        scheduling_queue_entry.valid = false;
        scheduling_queue_entry.next_valid = false;
        scheduling_queue_entry.busy[0] = false;
        scheduling_queue_entry.busy[1] = false;
        scheduling_queue_entry.next_busy[0] = false;
        scheduling_queue_entry.next_busy[1] = false;
        scheduling_queue.push_back(scheduling_queue_entry);
        valid_register.push_back(false);
    }

    /*Initialize the credits for each functional unit*/
    execute_count[0] = processor.k0_size;
    execute_count[1] = processor.k1_size;
    execute_count[2] = processor.k2_size;

    gen_tag = 1;
    num_fired_instructions = 0;
    cycles_counter = 1;
}

/*Fetch*/
/*Return false if fetch to the end of the trace file*/
bool fetch_inst(proc_inst_t* p_inst)
{
    bool ret;

    /*read the instructions from the trace file with fetch width*/
    for (uint64_t i = 0; i < processor.fetch_width; i++)
    {
        ret = read_instruction(&p_inst[i]);
        //printf("instructions: %d\n", p_inst[i].instruction_address);
        debug_table[0][instruction_counter[0]] = instruction_counter[0];
        debug_table[1][instruction_counter[0]] = cycles_counter;
        instruction_counter[0]++;
    }

    return ret;
}

/*Dispatch*/
void dispatch_queue_push(proc_inst_t* p_inst, bool start_dispatch)
{
    /*For simulating pipeline registers, 
    don't start if the first fetch is not finished and 
    stop dispatching if fetch stops*/
    if (!start_dispatch)
    {
        return;
    }

    /*push all the instructiosn into the dispatch queue
      create an entry in rob
      generate tag for the instruction*/
    for (uint64_t i = 0; i < processor.fetch_width; i++)
    {
        p_inst[i].tag = gen_tag;
        dispatch_queue.push_back(p_inst[i]);
        proc_rob_entry_t rob_entry;
        rob_entry.inst = p_inst[i];
        rob_entry.inst.tag = gen_tag;
        
        rob_entry.finish = false;
        ROB.push_back(rob_entry);
        

        debug_table[2][gen_tag] = cycles_counter;
        gen_tag++;
    }

    if (dispatch_queue.size() > max_dispatch_queue_size)
    {
        max_dispatch_queue_size = dispatch_queue.size();
    }
}

/*Check if there is an available slot in the reservation station
  Valid bit in scheduling queue indicates if the slot is available*/
int32_t rs_available()
{
    for (uint64_t i = 0; i < scheduling_queue.size(); i++)
    {
        if (!scheduling_queue[i].valid)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

/*Itearate over the ROB to find the entry index with the instruction tag*/
int ROB_lookup(uint64_t tag)
{
    for (uint64_t i = 0; i < ROB.size(); i++)
    {
        if (ROB[i].inst.tag == tag)
        {
            return (int) i;
        }
    }
    printf("ERROR! Instruction tag %ld not found in ROB!\n", tag);
    return -1;
}

/*Check for RAW dependency for SRC register 0*/
int check_data_dependency_0(proc_inst_t* inst)
{
    int ROB_index = ROB_lookup(inst->tag);

    /*The first entry in ROB won't have dependency*/
    if (ROB_index < 1)
    {
        return -1;
    }

    /*Iterate upwards in ROB from the current instruction*/
    for (int i = ROB_index - 1; i >= 0; i--)
    {
        /*if ROB entry has no destination register, skip*/
        if ((ROB[i].inst.dest_reg != -1)){
            /*if SRC register 0 equals ROB entry detination register and the ROB entry finishes then no deppendency
              else return the index of the ROB entry of the dependent instruction*/
            if ((ROB[i].inst.dest_reg == inst->src_reg[0]))
            {
                if (ROB[i].finish)
                {
                    return -1;
                }
                else {
                    return (int) i;
                }
            }
        }
        
    }
    return -1;
}

/*Check for RAW dependency for SRC register 1*/
int check_data_dependency_1(proc_inst_t* inst)
{
    int ROB_index = ROB_lookup(inst->tag);
    if (ROB_index < 1)
    {
        return -1;
    }
    for (int i = ROB_index - 1; i >= 0; i--)
    {
        if ((ROB[i].inst.dest_reg != -1)){
            if ((ROB[i].inst.dest_reg == inst->src_reg[1]))
            {
                if (ROB[i].finish)
                {
                    return -1;
                }
                else {
                    return (int) i;
                }
            }
        }
        
    }
    return -1;
}

/*Schedule*/
void schedule_inst()
{
    /*if there is no more instruction in dispatch queue, stop scheduling*/
    if (dispatch_queue.size() == 0)
    {
        return;
    }
    
    /*Find an available slot in schdeuling queue*/
    int32_t available_index = rs_available();

    /*If there is slot in scheduling queue and instruction waiting for dispatch*/
    while ((available_index >= 0) && (!dispatch_queue.empty())){
        proc_rs_entry_t new_entry;
        new_entry.inst = dispatch_queue[0];

        /*Check dependency for both SRC register*/
        int data_dependency_0 = check_data_dependency_0(&dispatch_queue[0]);
        int data_dependency_1 = check_data_dependency_1(&dispatch_queue[0]);

        /*Next busy is for simulating register behaviour
          Record the blocking instruction tag for each SRC registers*/
        if (data_dependency_0 != -1)
        {
            new_entry.busy[0] = true;
            new_entry.next_busy[0] = true;
            new_entry.blocking_inst[0] = ROB[data_dependency_0].inst.tag;
        }
        else
        {
            new_entry.busy[0] = false;
        }
        if (data_dependency_1 != -1)
        {
            new_entry.busy[1] = true;
            new_entry.next_busy[1] = true;
            new_entry.blocking_inst[1] = ROB[data_dependency_1].inst.tag;
            // printf("blocking inst tag: %ld\n", new_entry.blocking_inst);
        }
        else
        {
            new_entry.busy[1] = false;
        }

        /*Next valid is for simulation register behaviour*/
        new_entry.valid = true;
        new_entry.next_valid = true;
        valid_register[available_index] = true;
        new_entry.executed = false;

        scheduling_queue[available_index] = new_entry;
        dispatch_queue.erase(dispatch_queue.begin());
        debug_table[3][new_entry.inst.tag] = cycles_counter;
        available_index = rs_available();
    }
    
    return;
}

/*wake up the instructions waiting for dependent instructions in scheduling queue*/
void wakeup_scheduling_queue(proc_rob_entry_t* rob_entry)
{
    for (uint64_t k = 0; k < scheduling_queue.size(); k++)
    {
        /*Deassert the busy bits for the SRC registers separately*/
        if ((scheduling_queue[k].blocking_inst[0] == rob_entry->inst.tag) && (scheduling_queue[k].busy[0]) && (scheduling_queue[k].valid) && (!(scheduling_queue[k].executed)))
        {
            scheduling_queue[k].next_busy[0] = false;
        }
        if ((scheduling_queue[k].blocking_inst[1] == rob_entry->inst.tag) && (scheduling_queue[k].busy[1]) && (scheduling_queue[k].valid) && (!(scheduling_queue[k].executed)))
        {
            scheduling_queue[k].next_busy[1] = false;
        }
    }
}

// Comparator function
bool comp(proc_exec_entry_t a, proc_exec_entry_t b) {
    return a.tag < b.tag;
}

/*Execute*/
void execute()
{
    /*If scheduling queue is empty, stop execution*/
    bool continue_execute = false;
    for (uint64_t i = 0; i < scheduling_queue.size(); i++)
    {
        if (scheduling_queue[i].valid)
        {
            continue_execute = true;
            break;
        }
    }
    if (continue_execute == false)
    {
        return;
    }

    /*create an execution list storing the instructiosn executed for this cycle*/
    std::vector<proc_exec_entry_t> execution_list;
    for (uint64_t i = 0; i < scheduling_queue.size(); i++)
    {
        /*Select executable instructiosn only if both of the SRC registers are ready*/
        if (scheduling_queue[i].valid && ((!scheduling_queue[i].busy[0]) && (!scheduling_queue[i].busy[1])) && (!scheduling_queue[i].executed))
        {
            proc_exec_entry_t exec_entry;
            exec_entry.index = i;
            exec_entry.tag = scheduling_queue[i].inst.tag;
            execution_list.push_back(exec_entry);
        }
        
    }
    /*Sort the executed instructions in tag ascending order*/
    sort(execution_list.begin(), execution_list.end(), comp);
    
    /*Check for functional unit dependency and update the functional unit credit
      Push the executed instructions into a global result bus shifter*/
    for (uint64_t i = 0; i < execution_list.size(); i++)
    {
        int index = execution_list[i].index;
        int func_unit = scheduling_queue[index].inst.op_code;
        if (scheduling_queue[index].inst.op_code == -1)
        {
            func_unit = 1;
        }
        if (execute_count[func_unit] == 0)
        {
            continue;
        }
        scheduling_queue[index].executed = true;
        num_fired_instructions++;

        debug_table[4][scheduling_queue[index].inst.tag] = cycles_counter;

        execute_count[func_unit]--;

        result_bus_shifter.push(scheduling_queue[index]);
    }
    
    return;
}


/*Commit the ROB entry*/
void ROB_commit(proc_stats_t* p_stats)
{
    ROB.erase(ROB.begin());
    p_stats->retired_instruction++;
}

/*State Update*/
void state_update(proc_stats_t* p_stats)
{
    /*Simulating the busy bits and ready bit register behaviour*/
    for (uint64_t i = 0; i < scheduling_queue.size(); i++)
    {
        if ((scheduling_queue[i].busy[0]) && (!scheduling_queue[i].next_busy[0]) && (scheduling_queue[i].valid))
        {
            scheduling_queue[i].busy[0] = false;
        }
        if ((scheduling_queue[i].busy[1]) && (!scheduling_queue[i].next_busy[1]) && (scheduling_queue[i].valid))
        {
            scheduling_queue[i].busy[1] = false;
        }
        if (!valid_register[i])
        {
            scheduling_queue[i].valid = false;
        }
        if (!scheduling_queue[i].next_valid)
        {
            valid_register[i] = false;
        }
    }

    /*if the result bus shifter is empty, stop updating the state*/
    if (result_bus_shifter.empty())
    {
        return;
    }
    
    /*Update the state with only the common data bus width*/
    for (uint64_t j = 0; j < processor.cdb_size; j++)
    {
        if (result_bus_shifter.empty())
        {
            break;
        }
        proc_rs_entry_t finished_entry = result_bus_shifter.front();
        result_bus_shifter.pop();
        for (uint64_t i = 0; i < scheduling_queue.size(); i++)
        {
            if (finished_entry.inst.tag == scheduling_queue[i].inst.tag){
                scheduling_queue[i].next_valid = false;

                int ROB_index = ROB_lookup(scheduling_queue[i].inst.tag);
                ROB[ROB_index].finish = true;
                int func_unit = ROB[ROB_index].inst.op_code;
                if (func_unit == -1)
                {
                    func_unit = 1;
                }
                /*Release the functional unit credit
                  wake up the dependent instructions*/
                execute_count[func_unit]++;
                wakeup_scheduling_queue(&ROB[ROB_index]);
                debug_table[5][ROB[ROB_index].inst.tag] = cycles_counter;
                break;
            }
        }
    }

    while(ROB.front().finish)
    {
        ROB_commit(p_stats);
        if (ROB.empty())
        {
            break;
        }
    }

    return;
}
/**
 * Subroutine that simulates the processor.
 *   The processor should fetch instructions as appropriate, until all instructions have executed
 * XXX: You're responsible for completing this routine
 *
 * @p_stats Pointer to the statistics structure
 */
void run_proc(proc_stats_t* p_stats)
{
    proc_inst_t fetch_instructions[processor.fetch_width];
    proc_inst_t dispatch_instructions[processor.fetch_width];

    bool start_trace = true;
    bool start_dispatch = false;
    while(true){
    // for (int i = 0; i < 20; i++){
        state_update(p_stats);
        execute();
        schedule_inst();
        dispatch_queue_push(dispatch_instructions, start_dispatch);
        average_dispatch_queue_size += dispatch_queue.size();
        /*Simulation pipeline register behaviour*/
        if (start_trace){
            start_trace = fetch_inst(fetch_instructions);
            if (start_trace){
                memcpy(&dispatch_instructions, &fetch_instructions, sizeof(fetch_instructions));
                start_dispatch = true;
            } else {
                start_dispatch = false;
            }
        } else {
            start_dispatch = false;
        }
        p_stats->cycle_count++;
        cycles_counter++;
        if (p_stats->retired_instruction == 100000)
        {
            break;
        }
    }
}

/**
 * Subroutine for cleaning up any outstanding instructions and calculating overall statistics
 * such as average IPC, average fire rate etc.
 * XXX: You're responsible for completing this routine
 *
 * @p_stats Pointer to the statistics structure
 */
void complete_proc(proc_stats_t *p_stats) 
{
    p_stats->max_disp_size = max_dispatch_queue_size;
    p_stats->avg_disp_size = (float) average_dispatch_queue_size / (float) p_stats->cycle_count;
    p_stats->avg_inst_fired = (float) num_fired_instructions / (float) p_stats->cycle_count;
    p_stats->avg_inst_retired = (float) p_stats->retired_instruction / (float) p_stats->cycle_count;
    // printf("INST	FETCH	DISP	SCHED	EXEC	STATE\n");
    // for (int i = 1; i < 41; i++)
    // {
    //     for (int j = 0; j < 6; j++)
    //     {
    //         printf("%d  ", debug_table[j][i]);
    //     }
    //     printf("\n");
    // }
    // Open a file
    ofstream file("mcf.output");
    file << "Processor Settings\n";
    file << "R: " << processor.cdb_size << "\n";
    file << "k0: " << processor.k0_size << "\n";
    file << "k1: " << processor.k1_size << "\n";
    file << "k2: " << processor.k2_size << "\n";
    file << "F: "  << processor.fetch_width << "\n";
    file << "\n";
    // Write the string to the file
    file << "INST	FETCH	DISP	SCHED	EXEC	STATE\n";
    for (int i = 1; i < 100001; i++)
    {
        for (int j = 0; j < 5; j++)
        {
            file << debug_table[j][i] << "	";
        }
        file << debug_table[5][i];
        file <<"\n";
    }
    file << "\n";
    file << "Processor stats:\n";
	file << "Total instructions: " << p_stats->retired_instruction << "\n";
    file << "Avg Dispatch queue size: " << fixed << setprecision(6) << p_stats->avg_disp_size << "\n";
    file << "Maximum Dispatch queue size: " << p_stats->max_disp_size << "\n";
    file << "Avg inst fired per cycle: " << p_stats->avg_inst_fired << "\n";
	file << "Avg inst retired per cycle: " << p_stats->avg_inst_retired << "\n";
	file << "Total run time (cycles): " << p_stats->cycle_count << "\n";
    file.close();
}
