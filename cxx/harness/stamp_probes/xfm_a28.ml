let r = ref 0;;
let g ~f (_ : Marshal.extern_flags) = f
let _ = g ~f:1 Marshal.Closures
