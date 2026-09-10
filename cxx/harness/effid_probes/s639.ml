(* S421 RESIDUAL (open, pre-existing -- NOT introduced by the s636 fix; it
   fails identically under NOINCALIASPRESENT=1).  Reading a value THROUGH the
   include-strengthened alias -- `S639n.Set.cardinal`, rather than through the
   original submodule `S639n.M.Set` -- leaves unresolved binders in our
   -dlambda (`?cardinal/N`, the qmark.sh signature) and segfaults.  The .cmi
   now says the member is Mp_present and takes a field; the consumer still
   re-roots the path as if it were transparent. *)
let () = Printf.printf "%d\n" (S639n.Set.cardinal (S639n.Set.singleton "a"))
