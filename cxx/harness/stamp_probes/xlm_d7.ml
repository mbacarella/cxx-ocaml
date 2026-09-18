let g () =
   let module N = Map.Make(String) in
   ()
let f () =
   let module N = Map.Make(String) in
   N.add "sum" 41 N.empty
