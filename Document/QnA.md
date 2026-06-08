## [Architecture Questions](#architecture-questions)

1. [Why did you choose a pipeline architecture?](#why-pipeline-architecture)
1. [Why not a single-threaded monolithic loop?](#why-pipeline-architecture)
1. [Why SPSC queues instead of MPSC or MPMC?]()
1. [What is the slowest stage?](#what-is-the-slowest-stage)
1. [How do you handle backpressure?](#how-do-you-handle-backpressure)
1. [What happens if the Filter stage is slower than Generator?](#why-pipeline-architecture)
1. [What happens if Labelling becomes the bottleneck?](#why-pipeline-architecture)
1. [Why do you need both linear and multi-threaded modes?](#why-do-you-need-both-linear-and-multi-threaded-modes)
1. [How do you prove the threaded version produces the same result as linear mode?](#why-pipeline-architecture)

---

## [C++17 Questions](#c17-questions-1)

1. [Where do you avoid heap allocation?](#where-do-you-avoid-heap-allocation)
1. [How do move semantics help in packet passing?](#how-do-move-semantics-help-in-packet-passing)
1. [Are packet structs trivially copyable?](#are-packet-structs-trivially-copyable)
1. [Why use alignas(64)?](#why-use-alignas64)
1. [What is false sharing?](#what-is-false-sharing)
1. [What is the Rule of Zero/Five in your classes?](#what-is-the-rule-of-zerofive-in-your-classes)
1. [Are destructors thread-safe?](#are-destructors-thread-safe)
1. [How do you handle exceptions inside worker threads?](#how-do-you-handle-exceptions-inside-worker-threads)
1. [Why use dependency injection for config and queues?](#why-use-dependency-injection-for-config-and-queues)
1. [Why avoid virtual dispatch in the threshold hot path?](#why-avoid-virtual-dispatch-in-the-threshold-hot-path)

---
## [Lock-Free / Concurrency Questions](#lock-free--concurrency-questions-1)

1. [What makes your queue lock-free?]()
1. [Is your queue wait-free or lock-free?]()
1. [What memory ordering do you use and why?]()
1. [What happens-before relation exists between producer and consumer?]()
1. [Why is SPSC easier than MPMC?]()
1. [Do you need CAS in SPSC?]()
1. [What are ABA risks here?]()
1. [How do you safely stop threads?]()
1. [What happens if the queue is full?]()
1. [What are the risks of busy waiting?]()

---

## Cache and CPU Questions 
---
1. [How does cache line alignment help?]()
1. [How do you avoid false sharing?]()
1. [What is spatial locality in the Filter block?]()
1. [What is temporal locality in Labelling?]()
1. [How would you identify cache misses?]()
1. [What is the cost of moving cache lines between cores?]()
1. [Why can threaded mode be slower per packet?]()
1. [How does branch prediction affect convolution and labelling?]()
1. [What is instruction-cache pressure?]()
1. [What data structures are L1-resident?]()

---
## Threshold / Convolution Questions
---
1. [Why provide a fixed default kernel?]()
1. [Why is fixed kernel faster than configurable kernel?]()
1. [Can the convolution be vectorized?]()
1. [What is complexity per packet?]()
1. [How does kernel size affect latency?]()
1. [Why use a sliding window?]()
1. [How are row boundaries handled?]()
1. [What is the cost of boundary policy?]()
1. [Why avoid shifting in the window?]()
1. [What happens with non-standard kernel sizes?]()

---
## Labelling / Union-Find Questions
---
1. [Why Union-Find?]()

1. [What is the complexity of find() and unite()?]()

“In textbook Union-Find with path compression and union-by-rank, both find() and unite() are amortized O(α(n)), where α(n) is the inverse Ackermann function, effectively constant for any realistic input.”

“In our implementation, find() uses path halving, so repeated lookups flatten the parent chain. unite() calls find() on both labels and then merges the higher label under the lower label so that label assignment is deterministic and matches the spec’s lower-label-survives behavior.”

“So practically, both operations are constant time for our system because the label count is bounded by m/2 and stored as uint16_t. One nuance is that our unite() is lower-label-rooted rather than pure union-by-rank, so the strict textbook bound depends mainly on path halving and the bounded problem size. If we wanted the strongest theoretical DSU bound, we could use true union-by-rank or union-by-size, but that would sacrifice deterministic lower-label survival unless we decouple canonical label from tree root.”

1. [Why path-halving?]()
-

“Path-halving is done in LabelMap::find() at the line parent_[label] = parent_[parent_[label]]. When a stored label points through a chain of parent labels, path-halving shortens that chain while resolving the canonical root. This matters because assignLabel() calls find() for every causal neighbour — NW, N, NE, and W — and unite() also calls find() before merging. So every merge not only connects components, but future neighbour lookups become cheaper.”

“For example, if row buffers contain label 2 but 2 has already been merged into 1, neighbourN() calls find(2) and returns 1. That lets us avoid relabelling old row-buffer entries. During a bridge pixel, assignLabel() sees roots like [1, 0, 2, 1], chooses 1 as the surviving label, calls unite(2, 1), and unite() updates parent_[2] = 1. The emitted LabelledPacket carries merge_old = 2 and merge_new = 1 so Tracing can merge accumulators too.”

[Why union-by-rank or lower-label-root?]()
-
we use the lower label root  or~~ union by rank~~ `“We use lower-label-rooting for deterministic canonical labels, combined with path-halving in find() for performance.”`statrgy for the merge events and label keeping because this way we dont have to keep updating  the previous pixels, we also get the benifit of the path halving which gives a ~~constant time find and merge of events~~ `“Effectively constant in practice, formally near-constant / amortized O(α(n)) when using compression-style optimizations.”`

Gap 2: You did not explain why lower label survives
The reason is not only performance.
The main reasons are:

1. deterministic output
1. easier debugging/testing
1. matches spec examples
1. stable canonical label for merge events
1. simpler tracing semantics: merge_old = higher, merge_new = lower



Gap 3: You did not explain the trade-off

Lower-label-rooting may create less optimal trees than true union-by-rank.
But your code uses path-halving, and the label space is bounded by:


[How do you handle merges?]()
-
“A merge happens when the current foreground pixel sees more than one distinct non-zero canonical root among its four causal neighbours: NW, N, NE, and W. In assignLabel(), each neighbour is read from RowLabelBuffer and resolved through label_map_.find(), so we compare canonical roots rather than stale labels.”

“We choose the smallest root as the surviving label, assign that to the current pixel, and call label_map_.unite() for every other distinct root. For example, if roots are [1, 0, 2, 1], label 1 survives and label 2 is absorbed, so parent_[2] becomes 1. Future find(2) calls return 1, so we do not need to relabel old pixels.”

“We also emit a merge event: merge_old is the absorbed label and merge_new is the surviving label. Tracing uses that event to merge blob accumulators. So the Union-Find update maintains labelling correctness, and the merge event keeps downstream tracing consistent.”

[Why store only active labels?]()
-
We are using the staergy to only active labels is because we know the throtical limit of active labels thhat is m/2 , but as we are more or less going to create a infinite row of we were to store the inactive labels at one time the cost of the find will be too great 

another reason is that once a label becomes inactive it stops growing so what ever label we had given it can be freezed and only tracing block  has the job of creating the blob once the event is emitted to it 


1. [How do you prevent unbounded memory growth?]()


1. [What edge cases exist across row boundaries?]()


1. [How do you handle sparse versus dense data?]()


1. [How do you prove active labels are not recycled too early?]()

---

## LabelMap Replication Questions
---

1. [Why does TracingBlock maintain its own LabelMap?]()
1. [Why not share a LabelMap?]()
1. [Why would mutex-protected find() / unite() hurt latency?]()
1. [How do replayed merge events preserve correctness?]()
1. [What invariant guarantees consistency?]()
1. [What happens if events are dropped?]()
1. [What if queue order is broken?]()
1. [Does SPSC ordering provide enough correctness?]()
1. [Do you need atomics inside the local LabelMap?]()
1. [How do you test replicated LabelMap correctness?]()

---
## Performance Measurement Questions
---

1. [How did you measure 150 ns and 500 ns?]()
1. [What clock did you use?]()
1. [Did you measure p99/p99.9?]()
1. [Did you pin threads?]()
1. [Did you disable logging?]()
1. [Did you remove allocation from the hot path?]()
1. [Did you measure queue latency separately?]()
1. [How do you know CSV parsing is not included?]()
1. [How do you know random generation cost is not dominating?]()
1. [How reproducible are the numbers?]()
---
## Scalability Questions
---

1. [How does the system scale with more columns?]()
1. [How does it scale with larger kernels?]()
1. [How does it scale with higher input frequency?]()
1. [How would you support multiple producers?]()
1. [How would you shard the labelling stage?]()
1. [What is the next bottleneck?]()
1. [What happens when BlobAccumulator no longer fits in L1?]()
1. [How does queue depth affect latency and drops?]()
1. [How do you handle NUMA?]()
1. [How would you port this to Linux or real-time Windows?]()

---
[Architecture Questions](#architecture-questions)
---
### [Why Pipeline Architecture?](#architecture-questions)


The pipeline architecture closely resembles that of  a assembly line in a factory where the product moves along a line and at each station a specific task is performed on it which brings it closer to the final product , similarly in our case the data packet is our product and the various stages of the pipeline are like the stations on the Assembly line, here each station/stage operates on the data packet and then passes it to the next stage for further processing.


All the stages are sequentially dependent on each other  but logically independent due to the Objective/Goal of each of the stage. And thus the Pipeline architecture felt the most natural fit for the problem statement as it allows us to decouple the stages and 

~~thus scale them independently~~ `“In this implementation, independent scaling mainly means that each stage can be optimized, measured, or assigned execution resources independently. It is not horizontal service scaling; it is stage-level concurrency and bottleneck isolation.”` while also allowing us to easily identify the bottlenecks and optimize them without affecting the other stages.

Just like a assembly line uses the conveyer belt in order to transport the product from one station to another we also use the queing mechanism in order to transport our data from one stage to another . These queues help decoupling the input and output stages. they also ensure that there is sequential processing of the data pacets as its a single producer and a single cinsumer for rach queue and thus we can easily identify the bottlenecks and optimize them without affecting the other stages.

Tradeoffs : the main tradeoff that i have experienced is the increased complexity of the code due to multithreading and synchronisation , ~~queing also experiences packet drops due to backpressure from the faster stages~~ `“In this design, if the generator reaches its deadline and the downstream queue is full, we drop the packet rather than blocking, because the timing contract is more important than processing every packet.”` and thus we need to keep track of the dropped packets and optimise them for the best performance.

```    
Follow-Up:
    “Why are packets being dropped? Is that acceptable?”

    Depending on the requirement, queues can be bounded.
    If loss is acceptable, we can drop or sample packets under backpressure.
    If loss is not acceptable, we should block producers,
    spill to disk, or use a durable queue
Improvement :
    Each block implement's a common interface like process(input) -> output,
    and each stage run's in its own worker thread consuming from its input queue and publishing to the output queue.
```

Now our flow looks like this :
Generator Block -> Queue 1 -> Threshold Block -> Queue 2 -> Labelling Block -> Queue 3 -> Tracing Block -> Output

- Pipeline architecture also makes the functiona and unit testing of each of the stage much easier and thus making it possiible to identify and fix bugs independently .

- It also makes it easier isolate the fualts or failures and  handle them in that specifc blocks gracefully.

- Throughput and latency can also measured easily and independently across the stages 


~~Though the requirements given for the problem itself kind of hinted towards a pipeline architecture as it mentioned the need for a generator block, threshold block, labelling block and tracing block which are all logically independent and thus can be easily decoupled using a pipeline architecture.~~

#### Expected Answer:

I chose pipeline architecture because the problem is naturally a streaming dataflow with causal dependencies. The generator produces pixel packets, the filter applies the 9-tap convolution and thresholding, the labeller assigns connected-component labels, and the tracer consumes labelled packets plus merge/recycle events to compute blob statistics.

The flow is:

    Plain TextGenerator → SPSC Queue → Filter/Threshold → SPSC Queue → Labelling → SPSC Queue → Tracing → Output

The stages are dependent in data flow, but independent in responsibility. That means each block can be developed, tested, measured, and optimized independently. This also avoids shared mutable state between stages.

`Queues are used because each channel has exactly one producer and one consumer, so lock-free SPSC queues are a good fit.` They preserve ordering between adjacent stages, avoid mutex overhead on the hot path, and allow pipeline overlap — while the filter processes one packet, the generator can produce the next one and the labeller/tracer can work on earlier packets.

`The` **`queues are bounded to m/2`**, `which satisfies the memory constraint and prevents unbounded buffering.` If a downstream queue is full, the generator drops the packet instead of blocking, because ***the design prioritizes preserving the timing contract T.***

Another reason for this architecture is observability. Queue depth, dropped packets, throughput, and per-stage latency tell us where the bottleneck is. For example, in large m runs, queue occupancy can show that labelling is the bottleneck due to row-boundary work and larger LabelMap/RowLabelBuffer cache pressure.


I also considered a linear single-threaded approach. In fact, I implemented it as a baseline to measure pure algorithmic cost without queue and thread overhead. The linear model is useful for profiling, but it does not provide pipeline overlap, backpressure handling, or stage-level concurrency. So the threaded pipeline is the production design, while the linear model is the measurement baseline.

The trade-off is increased complexity: multithreading, synchronization, bounded-queue backpressure, packet drops, and OS scheduling effects. But given the stage-based structure and the need for modularity, testing, timing visibility, and future extensibility, pipeline architecture fits this system best


### [Why not a single-threaded monolithic loop?](#architecture-questions)

The main reasons are 
1. Scalabiility :
    - as the baseline measurement for the algorithmic cost the single threaded loop is useful as it outperforms the Pipeline code  due not having the to deal with Scheduling overhead , synchronization ovverhead and queing policies.
    - But single threaded monolithic loop the processing of each packet is strictly sequencial and thus we cannot take the advantage of the stages being independent i.e. the pipeline overlap between the stages, ~~This creates a bottleneck at each stage handover as the delay adds up and thus the overall throughput is reduced.~~**`“All stage costs are serialized in one call path, so the total per-packet latency becomes the sum of generator + filter + labelling + tracing.”`**


**`The true Cost of the single threaded approach is that the, it does not give us pipeline overlap, stage-level concurrency, or production-style backpressure handling.`**

2. Modularity and Testability :
    - Since the Whole pipleline behavves like a single loop and not independent stages the testing has to be done for the whole pipeline and thus making it harder to identify and fix bugs
    - The Modularity is affected in same way as the Stages are impossible to decouple thus making the final outtput tightly coupled across each stage 
    - we can make  use of the indepent propertirs of the prosessing stages in order to make the Testing modular though just hard function testing but the integrity of the class is still hard to check.
3. Loose Coupling and Maintainbility:
    - as previously said the single loop makes all the stages tightly coupled and thus the maintainability of the code is affected as every change requires a thotough understanding of the whole code and thus making it harder to maintain and extend in future
4. Perfromance and Throughput :
    - Performance and throughput now depends oon the slowest stage and thus if one stage creates delay its observed and carried through wach of the packet till the last stage , such bottlenecks are hard to identify as they require analysis of the whole code and optimising the stages individually is not possible due to tight coupling.

    Misconception : The single threaded loop is faster than the multi-threaded pipeline due to lack of synchronization and queuing overhead. However, the linear model does not provide pipeline overlap, backpressure handling, or stage-level concurrency. So while the linear model is useful for profiling, ~~it does not meet the timing contract T~~ `“The linear model is actually useful for proving the algorithmic average timing, but it does not provide the production properties of the threaded pipeline.”`  or allow for modularity and testing of individual stages. The threaded pipeline is the production design, while the linear model is the measurement baseline.

        Mistakes :
        - I am trying to prove that the single threaded approach is worse compared to the multi-threaded version
        - Meanwhile as Baseline measurement the single threaded veriosn can show the alforthmic cost of the approach much better than multithreadded which is affected by OS scheduling and other factors
        - i have completed ignored the advantages of the approach like simplicity for implementation and debugging and how it doesnt have to worry about synchronozartion
        - Though the Task explicitly talks about the memory constraint of each stage linear doesnot have to worry about that due to the serialized processing of the data packets thus no queue backpressure , drops etc.

#### Expected Answer:

A single-threaded monolithic loop was a valid alternative, and I actually used the linear mode as a baseline. Its main advantage is that it avoids thread scheduling, synchronization, and queue overhead, so it gives a cleaner measurement of the raw algorithmic cost.

However, I did not choose it as the production architecture because it serializes the whole flow:

Generator → Filter/Threshold → Labelling → Tracing → Output

All stages execute in one thread and one control path, so there is no pipeline overlap. In the threaded pipeline, while the filter is processing packet K, the generator can produce packet K+1, and the labeller/tracer can process earlier packets. That overlap is the main production benefit.

A single-threaded loop also does not give production-style backpressure handling. There are no bounded inter-stage queues, no queue-depth signal, and no explicit drop/block policy between stages. In the threaded version, bounded SPSC queues make pressure visible through queue depth, peak occupancy, and dropped packet counts.

From a modularity and testing perspective, the stages can still be written as separate classes or functions in a linear loop, but runtime execution remains tightly coupled. The pipeline architecture maps more naturally to the responsibilities of the system: generation, filtering, labelling, and tracing can be developed, tested, measured, and optimized independently.

The trade-off is that the pipeline adds complexity: multiple threads, synchronization, queue sizing, backpressure handling, and OS scheduling effects. In fact, the linear mode can be faster for raw algorithm measurement. But for the production design, pipeline architecture provides stage-level concurrency, observability, fault isolation, bounded buffering, and better extensibility.



### [What is the slowest stage?](#architecture-questions)

Generator Block has a parameter T which should be taken as the baseline because we are controlling the throtlling of the sytem through the parameter and adjustingg the rate at which tht pipeline consumes the data.Thus the generative block seems well within the required  limits 

Filter Block and labelling block have a lot of tasks for they have to read the packet and then perform covolution and neighbour check and then again publish the data for the next block 

The filtering block is using the sliding window approach for optimisation , move semantics to reduce the computation timings 

For labelling block the we are using the Map based SPSC queue for storing the active label's and we are also checking the neighbours points of the current packet as welland we fectch the active lables thousgh the map now this may become a bottle neck in case of large m as the max amount of active lables can go upto m/2 and traversla of map  becomes a botlle neck but for upto 10k active symbols it wont really cause any problems for stoorage of the active symbols 

tracing wporks on more or less similar level so the slowest stage is hard to determine thorugh raw theory i have not practically measure the timings for each of the block but the difference linear and the mulithtreaded pipelin before the integration of the lableing and tracing created around 40-60 ns in linear and 100-150 ns in multithreaded 

`which probably translates more towards the role of filtering being comutationally heavy `

Generator is dependent on the parameter T for the data production and thus can be taken as the base line , but down stream stages alos depend upon the rate at which the packets are being emitted at small T means that Filter queue is overflowing and dropping packets.m also plays a huge role as the low T and high m means not only the generator is overproducing and the memory constraint are also getting utlised to limit. This pressure on the downstream makes them process the packets not fast enough


Filter Block uses the queue which is internally a array with circular buffer and with atomic head and tail thus making the queue pretty optimmised for consumption and the default path i.e standard kernel is hardcoded for efficiency and even on custom kernel hot paths it uses slidig window for caluclations 


labelling block uses the union find statergy to keep the track of the labels that it has found and for merging them later . in worst case the labelling stage will have to manage atleast m/2 labels and for large m this means that ~~they cannot be managed on L1 and L2 and spill from cache~~  `“At large m, the labelling data structures no longer fit in L1 and spill into L2, increasing cache latency.”` +  there also possibi;ity of cache misses due to pointer traversal of the structure thus creating hidden delays in the stage neighbour checks and other mehtod also create delays 

Queue depth is one of the important metrices which can looked at to determine how well the blocks are performing as the generator-> filter is almost always full for low T it shows the overproduce as well as the borderline consumption deficit if os scehduling delays are ignored 

for the filter to labelling queue the analysis shows that the queue is under less pressure here as the queue remains empty for a while this can point towars that the lablleing is able to consume the filtered packets at a good rate 

again tracing shows the full depth being utilised


---
`Expected Answer :`
-

The slowest stage depends on whether we are looking at linear algorithmic cost or threaded runtime behavior.

In linear mode, at small m=130, the Filter dominates because the 9-tap convolution costs around ~144 ns while labelling plus tracing add only ~50–60 ns. But at large m=13,000, Filter remains roughly constant because its sliding window is fixed-size, while Labelling and Tracing grow due to larger LabelMap, RowLabelBuffer, and BlobAccumulator working sets. At that scale, Labelling becomes the main bottleneck because of Union-Find work, causal-neighbour checks, row-boundary recycling, and L1-to-L2 cache spill.

In threaded mode, the bottleneck is not just algorithmic. The system also pays queue transfer, cache-coherence, and OS scheduler overhead. That overhead can be larger than the T budget itself.

The clearest bottleneck signal is queue occupancy. For example, at m=13,000 and T=500 ns, gen→filter was full, filter→label was nearly empty, and label→trace was full. At first glance, the sparse middle queue may look like Filter is slow, but the full adjacent queues show a Labelling-side stall. The labeller periodically stalls on row-boundary operations, and the label→trace queue backs up.

So my conclusion is:
- At small m, Filter is the main algorithmic cost.
- At large m, Labelling becomes the bottleneck.
- In threaded mode, OS scheduling and inter-thread delivery overhead can dominate the measured latency.



### [How do you handle backpressure?](#architecture-questions)

so we have a strict performance criterian which creates a strict constraint from generation to filtering to be done within 100ns with precious constraints and with generation to tracing within 500ns that is the between two packets this constraint combined with the memory constraint with we can not use the unbounded queues between the stages as it will fillup the queues to quickly and create big backlog vioalting memory constraints and if we use the blocking queue then time constraint will be vioalated thus best statergy is to ensure that we comply with both the constraint by dropping the packets


### [Why do you need both linear and multi-threaded modes?](#architecture-questions)
"Linear mode is for measurement. Multi-threaded mode is for production realism."

“The multi-threaded mode is needed because the real system is a streaming pipeline where stages run concurrently and communicate through bounded queues.”

- linear Model :

    so linear model was implemented in order to stadardize or know the algorithmic cost associate with the processing as there are lot of factors which are affecting the processing time between the two consecutive packets like 
    - os scheduling and context switching of the Windows 
    - then trade offs with queue
        - packet drops 
        - ~~atomic operation though low cost compared~~ `SPSC queue does not need CAS on the normal data path` according to the design document; `it uses acquire/release atomics on head and tail`. **Peak occupancy tracking may use CAS**, but the**hot path is described as no mutex and no CAS on data path**. to mutex but the ~~CAS mechanics still consume some bandwith~~ `“Even lock-free queues have overhead: atomic loads/stores, memory ordering, cache-line movement, and producer-consumer synchronization.”`

thus the linear model is one way to ensure that we are able to get performance with just the algorithmic cost as it becomes a direct pass of the out put of ine stage to input of other  “In linear mode, the same transformations happen, but without inter-stage queues. In threaded mode, those transformations happen through packetized queue boundaries.”

`Expected Answer:`
-
“I need both modes because they answer two different performance questions.” -> i have understood what the question about

“The linear mode is a measurement baseline. It runs the same core algorithm in a single thread, without inter-stage queues, without synchronization, and without OS scheduling between stages. `So it tells me the raw algorithmic cost of the pipeline` — filtering, labelling, and tracing — without concurrency noise.” -> introduction to kinear model explaining goal and tradeoff

“The multi-threaded mode is the `production-like model`. In the real system, Generator, Filter, Labeller, and Tracer run as separate stages connected by bounded SPSC queues. That gives pipeline overlap, but `it also introduces real-world effects like queue push/pop cost`, atomic memory ordering, cache coherency, scheduling jitter, and backpressure.” -> introduction to multithreaded goal and trade off 

> main difference between the linear and multithread

“The important part is comparing both. If the linear mode 
- misses the timing budget, then the algorithm itself is too slow.  But if the linear mode passes and 

the multi-threaded mode fails, 
- then the issue is not the algorithm — it is threading overhead, 
- queue saturation, 
- OS scheduling, or 
- a slow stage creating backpressure.” 


“In our results, the linear mode showed that the algorithm can meet the average budget around T = 500 ns, while the threaded mode still had drops because the ***runtime overhead and scheduler behavior were significant***. That distinction is only possible because we have both modes.”

> Conclusion:

“So linear mode is for isolating algorithmic cost, and multi-threaded mode is for validating production behavior under real pipeline conditions.”



### [How do you prove the threaded version produces the same result as linear mode](#architecture-questions)

So, as said specifications provided it said that the appplication should have two modes first is csv mode and second is random 

to implement the random mode i have intriduced a configurable element in the mode called seed when this in config you keep this as 0 it will trigger a random infinite matrix generation everytime but for any non zero positive value the seed will create the same randome values in series 


this  a property of the rng module that is part of the standard c++ library collection which also enusre that this is within the no external library constraint 

now given that we are suing the config parameter seed as a non zero value  , the main difference are the packet drops in the multuthreaded mode which means that with packets dropped while processing the it may be that the final output labels and traces produced may not match which depends totally upon the m and T parameter used also if try to store generator output it also introduces latency spike due to wite operations




## [C++17 Questions](#c17-questions)

## [Where do you avoid heap allocation?](#c17-questions)
> Attempt for answer

When it comes to low latency sytems its generally adaviced to avoid heap allocation when it comes to hot paths or frequently accessed parts of the logic 

heap allocation is generally avoided due to overhead of the dynamic memory allocation mechanism , which includes structure like vectors maps set and tree etc which can grow dyncamically at run .

its adviced against thier use because of the heap fragmentation as well as at runtime we allocate deallocate too many heap objects it start causing overhead due to scanning of virtual memory bu the system calls which creates the overhead 

also the ~~hidden system call for allocation and deallocation is one the reasons~~ > `“The allocator may not call the OS every time, but it can still introduce unpredictable latency through locks, metadata updates, cache misses, fragmentation, and occasional page faults or arena growth.”` and hidden cost of the heap usage though modern runtime dont always use heap allocation but its still frown upon 

i have also tried to keep my code free of heap allocation on hot paths i have used them while creating the kernel array and even in that i have kept onne of the array as static array fixed at compile time which is default kernel and other allocated once during the intiliasation of the generator class i.e. ~~the dynamic kernel which is allocated once~~ `“The kernel comes from SystemConfig and is allocated/configured before the filter hot path runs. FilterBlock::dotProduct() only reads from config_.kernel; it does not allocate.”`and then used for the whole life time 

for Generator and filter shared queue i have also used a round buffer made of array which is also has fixed size at intilisation through the config and after that its fixed and resued for the whole duration of the application 

for filter and labelling block the same properties are followed but filter block containing the dynamic kwernel that is intiated dureing class contruction and from config along with the labelling block requiring the 2 rows of data in  order to make the decisions of so those are also heap allocated 

~~labelling and tracing block use the map and union find structurre in order to store the labelling and tracing data ~~`“Labelling and tracing both use a custom bounded LabelMap Union-Find, backed by preallocated arrays/vectors, not an associative map.”`

> Difference between heap allocation and heap-backed-storage

i have used heap basked storage for creating dynamic sized objects whose size is determined by config and after intilisation/constructor are called they remain fixed till end of the program so that we keep heap allocation away form the hotpaths and frequently executed logic 

the generator logic creating and emitting packets , filter logic where its consuming the packets from queue and performing convolution, labelling will traverse the neigbhours and then recombine/nerge labels or add them  these are the logical hot paths 

we have also made sure to reserve functionality of storage for active label storage thus also eliminating the use of temporary vector/list on the hot path 

    “I don’t claim the program is heap-free. The design is hot-path allocation-free. Heap-backed storage is allocated during construction/config loading, and then the per-packet path uses preallocated contiguous buffers or fixed packet structs.”

    "try to mention the exact hot paths"

The main allocation rule in this project is: allocate during construction, never during the per-packet hot path. The code is not completely heap-free — it uses heap-backed vectors and queues — but those structures are sized once from configuration and then reused.

In main.cpp, the three inter-stage queues are constructed once as DynamicSPSCQueue instances with logical depth m/2. During execution, queue operations only update atomic indices and copy fixed-size packets into preallocated ring slots. 

In the filter, the sliding window is constructed once, and processSample() only updates existing window slots, runs the unrolled 9-tap dot product, and stages output. There is no per-pixel allocation.

In labelling, LabelMap, RowLabelBuffer, pending_recycles_, and recycle_scratch_ are allocated or reserved in the constructor. That allows assignLabel() and mid-row recycling to run against preallocated storage. 

In tracing, the accumulator array is a flat vector indexed by label ID and constructed once. Merge, recycle, and pixel update operations mutate existing accumulators rather than allocating blob state dynamically. 

So the design trade-off is controlled heap usage: heap-backed contiguous storage for flexibility and cache locality, but no allocator activity in the hot path. That is what matters for predictable low-latency behavior.

## [How do move semantics help in packet passing?](#c17-questions)

~~all the structs used in project are trivial , and using static asser we also make the promise that they are both copyable and movable to enable compiler optimisation during runtime~~ -> its just the deifinition of the trvial structure and  not the actual inten behind the answer

>In this project, move semantics are not central to packet passing because packets are small, trivially copyable value types. Packet passing is intentionally based on cheap copies into preallocated SPSC ring-buffer slots, not ownership transfer via moves. 

> this shows 
- why move semantics why are not used 
- intent behind the trivial structure and copy property

but we haveexplicitly usedd the move semantics only for the transfer of constructed data source resource and in test cases 

`Expected Answer:`
-
Move semantics are not a major optimization for packet passing in this project because the packets are intentionally designed as small, fixed-layout, trivially copyable value types. `DataPacket, FilteredPacket, and LabelledPacket do not own heap memory; they just carry pixel values, coordinates, labels, flags, and event fields.` So moving them would be essentially the same cost as copying them.

The inter-stage queues use value-style transfer. A stage creates a packet locally, fills the fields, and pushes it into a preallocated SPSC queue slot. For example, FilterBlock::emitIfReady() fills a FilteredPacket and pushes it downstream, and LabellingBlock::emitPacket() does the same with LabelledPacket. There is no ownership transfer and no heap buffer inside the packet, so copy assignment is predictable and cheap. 

Where move semantics do matter is ownership setup, not packet transfer. createDataSource() returns a std::unique_ptr\<IDataSource\>, and main.cpp transfers it into GeneratorBlock using std::move. The GeneratorBlock constructor then stores it using source_(std::move(source)).` That avoids copying a polymorphic, resource-owning object and clearly expresses single ownership of the data source. `

*So my design choice is: use move semantics for resource ownership*, **but use trivially copyable fixed-size packets for the hot path.** `If packets later contained dynamic buffers or large payloads, then I would consider adding push(T&&) or move-only packet types,` but for the current design, copying small POD-like packets is simpler, safer, and fast.

Exact Sentences to Memorize
-

1. “Move semantics are important in this project for ownership transfer, not for packet transfer.”


1. “The packets are deliberately small and trivially copyable, so copying them into a ring-buffer slot is predictable and cheap.”


1. “A moved DataPacket would not be materially cheaper than a copied DataPacket because it owns no heap memory.”


1. “The real move-semantics example is std::unique_ptr<IDataSource> being moved into GeneratorBlock.”


1. “My rule here is: move resource-owning objects, copy fixed-size POD-like packets.”

## [Are packet structs trivially copyable?](#c17-questions)

Yes, i have kept the Packet strucutres trvially copiable as a design choice and kept thier size small to enable fast copies , ~~This method is also a standard practice when it comes to low latency systems to use copies instead of move semantics.~~ `In low-latency pipelines, small fixed-size packets are often copied by value because the copy is predictable, cache-friendly, and cheaper than managing ownership. Move semantics are more valuable when the object owns dynamic memory or another resource`

Move semantics are generally used where the ownership of large objects dynamic/heap allocated object which may aslo be polymorphic nature is concerned , for such objects the deep copies are expensive , thus ownership transfer through a move semantics are preffered 

We have used these trivially copiable structures as base for the Dynamic SPSC queue which have heap backed storage thus copies are cheap when we need to trasfer the packet from the produce  thread to consumer through the queue

in our implementaion there are three queues generator-> filter filter-> labelling and labeklling->tracing queue all of them are heap backed and allocated before use . ALSO Classes internally use ring bufferes (filter) and array/vector based maps(labelling and tracing ) which are also preallocated and structus can be triavally copied

Expected Answer :
-

Yes. The inter-stage packet structs are intentionally designed to be trivially copyable and small. The important packet types are DataPacket, FilteredPacket, and LabelledPacket; CompletedBlob is also trivially copyable for output/future queue use.

This matters because the production pipeline uses DynamicSPSCQueue<T\> for Generator → Filter, Filter → Labelling, and Labelling → Tracing. The queue implementation requires T to be trivially copyable and stores packets in a preallocated ring buffer. Push and pop are therefore fixed-size assignments into existing slots, not heap allocations or ownership transfers.

For these packets, move semantics would not give much benefit because the packets do not own heap memory. A moved DataPacket or LabelledPacket is effectively the same cost as a copied one. Move semantics are more useful elsewhere in the project, such as moving the std::unique_ptr<IDataSource\> into GeneratorBlock.

The design trade-off is intentional: packet passing uses small value records for predictable latency, while resource-owning objects are moved during setup. Internal structures like SlidingWindow, LabelMap, RowLabelBuffer, and tracing accumulators are preallocated and mutated in place, but they are not the packet structs being copied through the queues.

## [Why use alignas(64)?](#c17-questions)

So in cache heiarchy we have 3 cache's L1, L2 , L3 and then the virtual memory and there is two types access that is facilated that is temporal and spacial access and for L3 we have have a mix of both so the results/fetched memory are stored in cache line and its size depends upon the processor and hardware specification 

for my setup i have choosen the alignas 64 in order to cater the cacheline optimisation as my system has a cahceline size of 64 Now, why does this matter , when L2 or L1 takes the data for processing they use  these cache line as a reference data L3 is shared common between several cores and thread so when the any core and its thread is modifying the data it has to validate that the data is whether modified in that time or not 

So if the data is cache line aligned and all date realated to structure is present on the same cacheline or if they are on multiple lines but any other data which may be utilised from any other core is not present on that line in the case the ~~validation by the core succeds every time~~ `  “False sharing causes unnecessary cache-line invalidations because coherence operates at cache-line granularity, not variable granularity.  ”`thus saving the cycles to copy the data again from L3 to L1 or L2 

This hardware function which is most important in order to ensure that processing of thed data is atomic because if the same cache line contains data for multiple core then its possible that one core has modified some data that is shared so  if another core uses the data from same cache line which is already modififed should be invalid operation , thus this ~~CAS operation is a must~~  `“The queue uses acquire/release atomics for correctness and avoids CAS on the hot path. CAS is only used for peak occupancy diagnostics.”`

we have made sure that ~~all the data members of the class~~ 
```C++
// For Dynamic SPSC queue we align the 
alignas(CACHE_LINE) std::atomic<std::size_t> head_;
alignas(CACHE_LINE) std::atomic<std::size_t> tail_;
alignas(CACHE_LINE) std::atomic<std::size_t> peak_occupancy_;
```
This ensures that false sharing is minimized as the producer and consumer thread are only accessing head(producer) or tail(consumer)  at a time 
wherever used are cache aligned as well as the struct and data packets are also aligned this makes it optimised access for the cores 


Expected Answer :
-

I use alignas(64) in the queue implementation to reduce false sharing. On typical x86/x64 hardware, a cache line is 64 bytes, and cache coherence works at cache-line granularity. So if two unrelated variables sit on the same cache line and are written by different cores, those cores can invalidate each other’s cache lines even though they are not logically sharing the same variable.

In this project, that matters most in SPSCQueue and DynamicSPSCQueue. The producer primarily writes head_, while the consumer primarily writes tail_. If head_ and tail_ were adjacent on the same cache line, every push and pop could create unnecessary cache-line bouncing between producer and consumer cores. By declaring them as alignas(64), I try to place them on separate cache lines and reduce coherence traffic.

This is not about making the operations atomic — std::atomic provides atomicity and acquire/release ordering. The alignment is purely a performance optimization to avoid false sharing. Also, the SPSC queue intentionally avoids CAS on the push/pop hot path; CAS is only used in DynamicSPSCQueue for the diagnostic peak_occupancy_ high-water mark.

I would not apply alignas(64) blindly to every struct. Packet structs like DataPacket, FilteredPacket, and LabelledPacket are kept compact for cheap copying. Cache-line alignment is applied selectively to queue metadata that is frequently modified by different threads.

## [What is false sharing?](#c17-questions)

False sharing is phenomena where multiple cores are accssing/modifying their resppective data from the same cache line and because they end up modfying the data on the cache line this invalidates the copy taken by the other core 

So then they have to agian take copy form the cache line and again try to modify the cacheline since this is nonblocking operation its possible that is ~~copy modify and validate operation~~  `“The cores are not fighting over the same variable, but the hardware coherence protocol tracks ownership at cache-line granularity, so writes to one variable invalidate the whole line.”` may fail mutiple times and thus creating a ping pong type of situation between cores thus creating  a uknown lag 

this behaviour can avoided by ensuring that data which could be ~~accessed / modified by a thread or core is present on the single cache~~ `“For fields written by different threads, I separate them onto different cache lines. In the SPSC queue, producer-owned head_ and consumer-owned tail_ should not share a cache line.”` line like for Dynamic SPSC implementation we have kept the head tail and peak occupancy integer on seperate cahce line using the alignas instruction this reduces the false sharing instances 

## [What is the Rule of Zero/Five in your classes?](#c17-questions)

i have generally used only the rule of zero and three i have not used the rule of 5 thus not providing the move semantics to any of the classes and copy semantics are also provided to structs which are explicitly declaread as trivially copiable 

the move semanctics and copy are not enable because the Classes do not have the need for transfering the ownership expect for the IDataSource class ffrom which the Random and CSV data source are created for copy the once constructed all the dynamic SPSC queues dont really need to deep copy  their contents to create and use the copies

the most impoertatn fundamental unit are datapacket filterdatapacket and labeldata packet they are keot trivally copiable to make faster copies when adding to queue's and taking from them 

## [Are destructors thread-safe?](#c17-questions)

i have used the rule of zero for all the classes declared for the implementaion and i have used standard library containers like vector and standard type like int , float and  atomics <T\> and thus all the operation required like copy move or destrucutors are the directly from the standard library but since we have not done any explcicty heap allocation in any of the classes it **~~~STL structures are thier destructors are inherently thread safe~~~** `“The destructors are safe under the program’s lifetime discipline: all worker threads are stopped and joined before the objects they access are destroyed.”`

A common interviewer trap:

**std::atomic<bool> running_ or stop_flag_ helps coordinate the run loop, but it does not protect object destruction.**

“If running_ is atomic, can I destroy the object while the thread is still running?”

Answer:

**`No. The atomic flag only signals the thread to exit. You still must join the thread before destroying the object or any queues it references.`**

Expected Answer :
- 

The destructors themselves are not designed to be independently thread-safe. The project relies on a clear lifetime contract: stop the worker thread, drain the relevant queues, join the thread, and only then allow the objects to be destroyed.

Most classes follow the Rule of Zero, so resources are owned by standard library members like std::vector, std::unique_ptr, std::ifstream, and std::ofstream. That gives RAII cleanup, but RAII does not mean it is safe to destroy an object while another thread is using it.

In main.cpp, this is handled explicitly. The generator is stopped and joined first, then the Generator → Filter queue is drained, then the filter is stopped and joined, and the same pattern continues for labelling and tracing. After trace_thread.join(), no worker thread is accessing the blocks, queues, or tracing output, so normal destructors can run safely.

The atomic running_ or stop_flag_ fields only signal the run loops to exit; they do not protect object lifetime. Destroying a DynamicSPSCQueue, FilterBlock, or TracingBlock while its thread is still active would be undefined behavior. So the answer is: destructors are safe because of external shutdown ordering, not because STL destructors are inherently thread-safe.

I avoid manual heap management and hot-path allocator activity. I do not use raw new or delete for packet processing. The project does use heap-backed STL containers like std::vector in DynamicSPSCQueue, SlidingWindow, LabelMap, RowLabelBuffer, and TracingBlock, but those are allocated or reserved during construction and then reused.

From a latency perspective, STL allocation still counts as heap allocation if it happens during the hot path. So my claim is not that the program is heap-free; my claim is that the latency-critical path is allocation-free.

For shutdown, I also added a signal handler for Ctrl+C / terminal termination. The handler only sets an atomic shutdown flag; it does not perform cleanup inside the signal handler. The main thread observes the flag and performs the controlled shutdown sequence: stop stages, drain queues, join threads, and then let RAII destructors run. That is what makes cleanup safe.

## [How do you handle exceptions inside worker threads?](#c17-questions)

Actually, we have throw statements but they are only limited for the intilisation and constructor of the classes this was because in case any of the class encounter error during the construction that naturally means that the class is illformed and we must exit the process with proper logs that's why all exceptions thrown in the construction or initialisation of the classes direcly lead to exit 

i have made sure that there are no explicit throw statements in the code on hot paths, this was done to because and exception handelling creates a issues that is binary bloating , ~~disable of compiler optimisation~~ `“Even zero-cost exceptions are not zero-cost for binary size, unwind metadata, code layout, and predictability. More importantly, throwing during a latency-critical path destroys deterministic timing.”` and also code branches for exception paths thus reducing the efficiency for the low latency systems 

Expected Answer :
- 

The intended design is that exceptions are handled during startup, not inside the worker hot paths. ****Configuration and construction failures are treated as fail-fast errors.**** For example, invalid kernel size in FilterBlock, invalid column count in LabellingBlock or TracingBlock, and invalid CSV input are detected before the pipeline is expected to run.

Once the worker threads are running, the per-packet paths are designed not to throw. Queue operations return true or false; backpressure is handled by dropping or retrying, not by exceptions. Labelling and tracing use preallocated buffers, fixed packet fields, merge/recycle events, and noexcept helpers where appropriate.

The important caveat is that the current thread entry functions in main.cpp do not wrap run() in try/catch. `**So if an unexpected exception escapes GeneratorBlock::run(), FilterBlock::run(), LabellingBlock::run(), or TracingBlock::run(), the process would terminate.**` For this evaluation implementation, the assumption is that validated configuration prevents those paths.

For production, `I would harden this by wrapping every thread entry point in try/catch, capturing std::exception_ptr`, **`setting a shared shutdown flag,`**` stopping all stages, joining all threads, and then reporting the failure from the supervisor thread. That would preserve the current fail-fast behavior but avoid uncontrolled termination from a worker thread.

## [Why use dependency injection for config and queues?](#c17-questions)

Deep Answer
-


The pipeline is composed in main.cpp. main() loads SystemConfig, creates the three inter-stage queues, creates the data source and tracing output, and then injects those dependencies into GeneratorBlock, FilterBlock, LabellingBlock, and TracingBlock.

This keeps each stage focused on its own responsibility. FilterBlock does not know whether input came from CSV or random mode; it only consumes DataPacket from IQueue<DataPacket>. LabellingBlock does not know whether the upstream queue is mutex-based or lock-free; it only pops FilteredPacket. TracingBlock does not know whether blobs are discarded, written to CSV, or printed; it only calls ITracingOutput::emit().

The biggest benefit is testability. Because blocks depend on interfaces like IQueue<T>, tests can use SimpleQueue<T>, while production uses DynamicSPSCQueue<T>. That lets me test stages independently without launching the full threaded pipeline.

The second benefit is explicit ownership and lifetime. main.cpp is the composition root and owns the queues and top-level resources. The blocks hold references to dependencies that outlive them. The data source is the exception where ownership is transferred into GeneratorBlock using std::unique_ptr<IDataSource>.

The trade-off is abstraction overhead. IQueue<T> uses virtual calls, and in a nanosecond-level system that can matter. If I wanted to optimize further, I would template the block types on the queue implementation so the compiler could inline push() and pop(). But for this project, dependency injection was chosen to prioritize modularity, testability, and clean stage boundaries.

## [Why avoid virtual dispatch in the threshold hot path?](#c17-questions)

Virtual Dispatch actually come with their own set overhrad at run time as each call to the overriddedd function requires runtime resolution and that means ~~**the more the class is lower in the hiearchy of the inheritance the more overhead it creates**~~ “The cost is not inheritance depth. The cost is that the call target is resolved indirectly at runtime, which can prevent inlining and create branch prediction/instruction-cache overhead.”

 Virtual dispatch overhead does not grow because a class is “lower in the inheritance hierarchy.” A virtual call normally 
- uses one vtable lookup / indirect call regardless of inheritance depth.

The main costs are indirect 
- `branch prediction, `
- `lost inlining, `
- `poorer optimization, and `
- `possible instruction/cache effects.`

for Filterblock it already has  a computationaly heavy components like convolution and it if there is a **~~virtual dispatch any where it will likely create a overhead that's much worse~~** `“The threshold operation is so small that a virtual call could cost more than the work being abstracted.”`




[Lock-Free / Concurrency Questions](#lock-free--concurrency-questions)
-

[What makes your queue lock-free?](#lock-free--concurrency-questions)

So in order to call a function lock we cannot use any type of locking mecahnism that means we cant use the mutex or semaphores in order create a deterministic access to out queue , So we have implemented a bounded ring buffer through vector and we have 2 index's declared for the read and write access to this ring buffer 

The producer uses the writing index , so it first loads the write index stores the write object and then advannces/publishes the write index 

The consumer uses the reading index , so it loads the read index and then after reading the object it advacnes/publishes the reading index location 

the read and write index;s are kept 64 byte in order to ensure that the overflow is not possible for a whilw it also wases the peak occupancy calculations as wll as the head==tail -> empty queue and head-tail >= m/2 or capacity => that the queue is under  saturation 


[Is your queue wait-free or lock-free?](#lock-free--concurrency-questions)
-
~~**Queue implementation is lockfree**~~ *`“I describe my queue as lock-free/non-blocking. The individual try_push and try_pop operations are bounded in the SPSC case, but the pipeline as a whole is not wait-free because successful progress may depend on the other stage producing data or freeing space.”`*, so the hiearchy of concepts go's like this wait-free -> lock-free ->obstruction-free ->blocking 

so if we have implemented a waitfree algorithm it automatically means that the program is also lock-free , So to implement a wait free algorithm we require all the threads should be able to independently complete their task in bounded number of steps now our queue implementation , all threads are indepent according to the task they perform but the queue data is required for the task they perform thus they have to wait for data acquition each time thus the solution cannot be called wait-free 

But for lockfree programming atleast one of the threads should be able to complete thier task in  bounded number of steps so this closely follows our threading algo as aleast one of the thread is either able to consume or produce the data in the queue and we dont have to block the consumer or producer in order to push or pop values but occasionaly wait in case the queue is empty 

`Expected Answer:`
-
“The queue is best described as lock-free and non-blocking. The push() and pop() methods do not block; they either complete successfully or return false when the queue is full or empty. There are no mutexes, semaphores, or condition variables in the SPSC hot path.
In the fixed-capacity SPSCQueue, the core push/pop operations are bounded because there is one producer writing head_ and one consumer writing tail_. The producer only reads tail_ to check capacity, and the consumer only reads head_ to check availability. So there is no multi-writer contention on either index.
For DynamicSPSCQueue, I avoid claiming strict wait-free behaviour because peak_occupancy_ is updated with a CAS loop for observability. That CAS loop is not required for queue correctness, and the packet is published before the peak update. So the queue is lock-free/non-blocking, and the core SPSC data transfer path is bounded, but the fully instrumented implementation is not something I would market as strictly wait-free.”

[What memory ordering do you use and why?](#lock-free--concurrency-questions)
-
I have used relaxed , release and acquire memory ordering in my implementaion ,So relaxed ordering is used ~~**where we need to just need to read**~~ **`“Relaxed ordering is used where we don’t need synchronization with other threads — only atomic read/write without ordering guarantees.”`** ~~the value of the variable and not publish the value we have just read ~~

release ordering is used in code base where ever we are ready to publish the packet so generally we use it after the packet data is copied to buffer and we would like the other thread/consumer to know that packet is avaialble to be used 

acquire ordering is used ~~where we need to read the latest value~~ "`Acquire ensures that if we observe a value written with release, we also observe all writes that happened before that release.`" ~~that was stored before the previous release operation this means that they are generally used here for reading the tail or head position~~ 

`Expected Answer:`
-
I use memory_order_relaxed, memory_order_release, and memory_order_acquire in the SPSC queue to implement a correct and minimal synchronization protocol.

In the producer (push()), `I first write the packet into the ring buffer, and then I publish the new head_ index using memory_order_release.` This ensures that all writes to the packet happen-before the head_ update is visible to the consumer.

In the consumer (pop()), I read head_ using memory_order_acquire. `When the consumer sees the updated head_, the acquire semantics guarantee that it also sees all prior writes made by the producer before the release stor`e — including the packet data in the buffer.

**This release–acquire pair establishes a happens-before relationship between producer and consumer.**

I use `memory_order_relaxed` for operations **where no cross-thread synchronization is required**, such as reading my own thread’s index or updating diagnostic counters like peak_occupancy_. ***Relaxed ordering guarantees atomicity but does not enforce ordering or visibility.***

This approach avoids stronger memory orderings like sequential consistency, which would add unnecessary fences and reduce performance, while still ensuring correctness of packet publication and consumption.


[What happens-before relation exists between producer and consumer?](#lock-free--concurrency-questions)
-
before the relatin between producer and consumer is established we first 
- call and use the config manager to initlialise the parameters kernel , m and  T etc . parameters
- after parameters are initiliased we then initialise the DataPacketQueue , FilterPacketQueue and LabelingPacketQueue 
- After this we start the Threads for Generator Block, Filter Block, Labelling Block and Tracing Block
- Once the threads are started this is where the relation between the producer and consumer is etablish in each of the Queue 


[Why is SPSC easier than MPMC?](#lock-free--concurrency-questions)
-
the main problem with implementing the MPMC is the memory ordering though the model is not  a fit for value of "m" but for larger values MPSC could be applied but it will require a lot boundary handling in order to work properly as expected 

- When we use the MPSC we will have to create a row-wise model where each producer thread generates a sperate row  for consumer but these Producer create output for next stage then filter output will act as input to Labelling
- It's one thing to have complete previous row for SPSC , but when multiple Producers are involved multiple rows of threshold will be sent to consumer and then its impossible to have a proper ordering in all 4 rows and even if have n previous thread connecting those threads will not be possible 
- So we have to make the MPSC pattern in this which means we have to first have n completed row for which we perform the labelling and simultaneouly fill the rows 
- How to feed the tracing block is becoming hard to imagine as we are parsing the events for n rows meanwhile more n rows are added to queue 

[Do you need CAS in SPSC?](#lock-free--concurrency-questions)
-


[What are ABA risks here?](#lock-free--concurrency-questions)
-


[How do you safely stop threads?](#lock-free--concurrency-questions)
-


[What happens if the queue is full?](#lock-free--concurrency-questions)
-


[What are the risks of busy waiting?](#lock-free--concurrency-questions)
-



[How does cache line alignment help?]()

Cache line alignment helps in ensuring that head tail and peakoccupany can be placed on seperate cache line this reducces false sharing problem which occurs as multiple threads try to access the same cache line for thier respective independent variable as any of the threads tries to modif the data on the cache line , for other threads this line is now corrupted and this creates a ping pong effect  increaing latencyh spike

False sharing does not usually break correctness. It increases latency because the hardware keeps invalidating and transferring cache-line ownership between cores.


[How do you avoid false sharing?]()

cache line alignmment is main approach that is used to avoid the fasle sharing effect for our code base in Dynamic SPSC implementation we head and tail and peakoccupancy variable which are kept on seperate cache line this makes the Update possible without the false sharing problem 

amother design choice is that ~~producer and consumer have only read only and write only access~~ `“Each index has a single writer. The producer is the only writer of head_, and the consumer is the only writer of tail_. They may read the other index, but they do not both write the same index.”`to head and tail like producers writes to head and consumer writes to tail only this way we are enabling a low latency lack free algorithm

[What is spatial locality in the Filter block?]()

Spacial Lcality in the filterBlock is for the ringer Buffer SPSC queue, Kernel , and sliding window sum that we have along with the 


[What is temporal locality in Labelling?]()



[How would you identify cache misses?]()

[What is the cost of moving cache lines between cores?]()

[Why can threaded mode be slower per packet?]()

[How does branch prediction affect convolution and labelling?]()

[What is instruction-cache pressure?]()

[What data structures are L1-resident?]()