let r = ref 0;;
let g (_ : Marshal.extern_flags array) = ()
let _ = g [| Marshal.Closures |]
