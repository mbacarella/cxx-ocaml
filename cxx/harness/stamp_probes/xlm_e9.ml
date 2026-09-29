let g () =
   let module N = Map.Make(Int) in
   N.empty
let h () =
   let module N = Map.Make(String) in
   N.empty
let f () = 1
