let f () =
   let module N = Map.Make(String) in
   N.add "sum" 41 N.empty
let g () =
   let module N = Set.Make(String) in
   N.add "sum" N.empty
