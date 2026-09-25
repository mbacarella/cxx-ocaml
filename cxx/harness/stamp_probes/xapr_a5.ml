let f () = ()
let t = [| (fun () -> f ()); (fun () -> print_string "a") |]
