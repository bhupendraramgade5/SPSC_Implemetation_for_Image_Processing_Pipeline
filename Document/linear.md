# Linear Approach 

# Why ??

So when i was almost  ompleted witht he multithreaded approach i was pretty sure that the through put should more or less match the given contraints and i was way off on my estimations and thus i decieded to create a linear pipeline in order to measure the actual through put incase the generated item from the Random data ggenerator block or the Csv reader block was directly passed to the rthreshold block

The main idea was to see exactly where the time is being consumed since the both the thresold block and the generator did not seem to have any sort of computation which might create delay 

1. i have tried to keep the use of runtime polymorphism as minimum as possible as the calls to such function may have created small but measurable delays 
1. one thing i came to know about later was Link Time Optimisations which may have also impacted the Performance metrices in both modes 
1. i have watched several videos on how the false sharing can create runtime  slowdown and have used kept the data sources and output  cache lined align to eleminate those possibilities as much as possible 
1. 
### Generator Block 


When code is split into multiple translation units (.cpp files), interfaces, virtual layers, or shared libraries, visibility decreases.

As visibility decreases:

Inlining opportunities disappear
Constant propagation disappears
Alias analysis becomes more conservative
Vectorization opportunities disappear
Dead code elimination becomes weaker
Branch elimination becomes impossible
Devirtualization may fail