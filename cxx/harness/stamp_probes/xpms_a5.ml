module F (A : sig type t end) = struct
  let v : A.t option = None
end
