let f () =
   let module N = Set.Make(String) in
   match N.empty with _ -> ()
