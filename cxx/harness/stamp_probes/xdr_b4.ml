let f () =
  let module M =
    (struct module S = Set.Make (String) type u = S.t end :
       sig type u end)
  in
  ignore (0)
