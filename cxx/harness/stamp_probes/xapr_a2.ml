let f () = ()
let g () = print_string "a"
let t = [| (fun () -> f ()); g |]
