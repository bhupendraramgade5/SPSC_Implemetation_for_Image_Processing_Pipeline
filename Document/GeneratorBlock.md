# Generator Block :

### Introduction to File
Contains the Definition for the Random and CSV data with abstracted Parent that is IData Source so that it maked the introduction of the new Data Source/Mode or introduction of new methods more flexible and Scalable in Future 


### `std::unique_ptr<IDataSource> createDataSource(const SystemConfig& config);`

Create Data Source function is used for the creation of the data source object used by the generation at runtime it creates and returns a uniqueptr to the abstracted base class IDataSource and returns the data source based on the arguments passed through comand line or the config file 

it can create two types of Data Source:
1. Random Data Store
1. CSV data store 



## Generator Class :

### Members : 

`const SystemConfig& config_` :- 

 Object that stores intialisation parameters

`IQueue<DataPacket>& queue_` : 

Abstract class object so that queue_ object could easily be replaced in case we found  a better implementation or need to modify the internal functions 

`std::unique_ptr<IDataSource> source_` : 

incase we received intruption signal from the terminal in that case we need to shutdown the process gracefully\

`std::atomic<uint64_t> rows_emitted_{0} :` 

 counters for the number of rows emitted
`std::atomic<uint64_t> dropped_packets_{0}:` 

number of packets that were dropped duue to the Consumer not acting fast enough


### Functions : 
`void spinWaitUntil(std::chrono::steady_clock::time_point deadline) const: `

	custom spin lock for sleeping or stting idle for precise timings introduced to satisfy the "T" parameter mentioned in the Specification its a precision sleep for the working generator block wso that after generating each pixel it sleeps for exactly `**T nanoseconds**`

`void run();`

for Running the Generator Block thread and emitting the data packets to the queue.

`void stop();`

Signal Handler or Gracelfull shutdown function for the Generator Block thread in case of receiving intruption signal from the terminal or any other source or incase the Maximum number of rows allowed have been emiitted or the process has been running for more than the maximum allowed time limit.

