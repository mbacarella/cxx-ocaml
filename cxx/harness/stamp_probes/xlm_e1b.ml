type t = int Map.Make(String).t
let g () =
   let module N = Map.Make(String) in
   ()
let f () = 1
