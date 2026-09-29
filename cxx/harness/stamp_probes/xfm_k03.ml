let r = ref 0;;
let g a b = a = b
let f (x : Marshal.extern_flags) = g x x
