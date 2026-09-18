let f () =
   let module N = Set.Make(String) in
   let s = N.empty in
   let t = N.empty in
   N.equal s t
