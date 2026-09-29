let r = ref 0;;
let f b (x : Sys.backend_type) = if b then x else Sys.Native
