module type Total = sig
    type t
    val compare: t -> t -> int
end

module Create(P: Total) = struct
    module Q = struct module Priority = P end
end
module Basic = struct
    include Create(struct type t = int  let compare a b = b - a end)
end
