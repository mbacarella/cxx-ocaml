module type Total = sig
    type t
    module N : sig type s end
end
module Create(P: Total) = struct
    module Q = struct module Priority = P end
end
module Basic = struct
    module R = struct include Create(struct type t = int module N = struct
      type s = int end end) end
end
