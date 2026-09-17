let r = ref 0;;
let g (_ : Marshal.extern_flags option) = ()
let _ = g (Some Marshal.Closures)
