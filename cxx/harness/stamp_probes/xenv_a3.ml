(* a bare Stdlib toplevel type a record-field pattern resolved, with no
   provenance of its own: still the implicit open's one "ref" string *)
let g { contents = x } { contents = y } = x + y
let h { contents = z } = z
