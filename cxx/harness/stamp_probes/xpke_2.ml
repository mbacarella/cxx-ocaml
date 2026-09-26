module type E = sig exception Ex of string * int end let f (module M : E)
  x = match x with M.Ex (s, _) -> s | _ -> ""
