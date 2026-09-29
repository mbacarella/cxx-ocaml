module F (X : sig type t val v : t end) = struct
  let v = X.v
end
