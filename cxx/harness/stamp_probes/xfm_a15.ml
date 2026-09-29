let r = ref 0;;
let f (x : Marshal.extern_flags) = match x with Closures -> 0 | _ -> 1
