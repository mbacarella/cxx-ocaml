module type Total = sig
    type t
    val compare: t -> t -> int
end

module Create(P: Total) = struct
    module Priority = P
    type u = Priority.t
end
include Create(struct type t = int  let compare a b = b - a end)
