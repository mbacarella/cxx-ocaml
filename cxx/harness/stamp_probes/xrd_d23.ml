module type Total = sig
    type t
    module N : sig type s end
end
module Create(P: Total) = struct
    include P
end
module Basic = struct
    include Create(struct type t = int module N = struct type s = int end end)
end
