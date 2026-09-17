let r = ref 0;;
let g (_ : Marshal.extern_flags * int) = ()
let f (x : Marshal.extern_flags) = g (x, 1)
