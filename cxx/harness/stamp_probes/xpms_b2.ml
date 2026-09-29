module F (D : sig type t val v : t end) = struct
  open D
  let x : t list = [v]
end
