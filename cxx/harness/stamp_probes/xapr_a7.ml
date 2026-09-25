let g () = print_string "a"
let h () = ()
let t = [| [| h |]; [| g; h |] |]
