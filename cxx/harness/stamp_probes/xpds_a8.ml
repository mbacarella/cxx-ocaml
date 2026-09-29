(* a functor PARAMETER's members *)
module F (X : sig type t val z : t end) = struct
  let get = X.z
  let pair = (X.z, X.z)
end
