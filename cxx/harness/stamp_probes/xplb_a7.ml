module F (X : sig type t = {a : int; b : string} end) = struct
  let v x = X.(x.b)
end
