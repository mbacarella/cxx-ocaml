module F (X : sig end) (Y : sig end) = struct let mk () = 1 end
module B = struct end
module M = F(B)(B)
let f () = M.mk ()
