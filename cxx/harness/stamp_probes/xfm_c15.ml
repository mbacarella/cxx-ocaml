let r = ref 0;;
let _ = Gc.set { (Gc.get ()) with Gc.minor_heap_size = 1 }
