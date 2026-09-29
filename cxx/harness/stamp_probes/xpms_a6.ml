module F (X : sig module N : sig type t end end) = struct
  let v : X.N.t list = []
end
