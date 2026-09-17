module type Total = sig
    type t
    val compare: t -> t -> int
end

module Create(P: Total) = struct
    include P
end
module Basic = Create(struct type t = int  let compare a b = b - a end)
