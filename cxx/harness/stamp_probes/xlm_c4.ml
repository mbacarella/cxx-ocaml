let f () =
   let module N = Map.Make(String) in
   N.add "sum" 41 N.empty
let g () =
   let module N = Map.Make(Int) in
   N.add 1 41 N.empty
