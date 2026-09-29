type r = {a : int}
module F (X : sig end) = struct let v x = x.a end
