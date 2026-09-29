module type Total = sig
    type t
    module N : sig type s end
end
module Create(P: Total) = struct
    module Priority = P.N
end
module Basic = struct
    include Create(struct type t = int module N = struct type s = int end end)
end
