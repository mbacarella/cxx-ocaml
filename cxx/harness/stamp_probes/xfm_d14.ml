let r = ref 0;;
let f (x : Sys.backend_type) = match x with Sys.Native -> 1 | _ -> 2
