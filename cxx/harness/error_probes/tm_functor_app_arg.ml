module F (X : sig val x : int end) = struct end
module M = F(struct let x = "a" end)
