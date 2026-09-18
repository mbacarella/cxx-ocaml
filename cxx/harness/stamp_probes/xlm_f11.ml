let f b =
   let module N = Set.Make(String) in
   if b then N.empty else N.empty
