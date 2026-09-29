module M = Map.Make(Int)
type t = int Map.Make(String).t
let g () =
   let module N = Map.Make(String) in
   N.empty
let f () = 1
