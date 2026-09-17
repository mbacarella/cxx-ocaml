let r = ref 0;;
let g (_ : Marshal.extern_flags) = ()
let f () = g Marshal.Closures
