module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
        module N : sig type s val x : s end
    end
end

module type R = sig
    module Priority: Order.Total
    type u = Priority.t
end
module Pr : Order.Total = struct type t = int let compare = compare module N =
  struct type s = int let x = 1 end end
