let r = ref 0;;
let g ~(x : Marshal.extern_flags) = x
let _ = g ~x:Marshal.Closures
