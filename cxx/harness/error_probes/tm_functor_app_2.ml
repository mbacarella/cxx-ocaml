module F (X : sig val x : int end) (Y : sig val y : int end) = struct end
module M = F(struct let y = 1 end)
