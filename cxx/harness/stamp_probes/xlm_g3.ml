module X = String
let f () =
   let module N = Map.Make(X) in
   N.add "a" 41 N.empty
