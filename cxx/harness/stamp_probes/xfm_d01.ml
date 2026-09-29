let r = ref 0;;
let _ = Sys.set_signal 0 (Sys.Signal_handle (fun _ -> ()))
