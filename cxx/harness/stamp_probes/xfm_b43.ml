let r = ref 0;;
let f (x : Gc.control) = Gc.set x; x.Gc.minor_heap_size
