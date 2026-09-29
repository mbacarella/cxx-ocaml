let r = ref 0;;
let g : Marshal.extern_flags -> unit = fun _ -> ()
let _ = g Marshal.Closures
