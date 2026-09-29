let r = ref 0;;
let f (x : Gc.control) = { x with minor_heap_size = 1 }
