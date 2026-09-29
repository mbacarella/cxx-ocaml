module F (X : sig type t val v : t end) = struct let w = X.v end include
  F(struct type t = string let v = "" end) let y = w
