let r = ref 0;;
let g (_ : Marshal.extern_flags * int) = ()
let _ = g (Marshal.Closures, 1)
