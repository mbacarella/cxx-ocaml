module Order = struct
    module type Total = sig
        type t
        val compare: t -> t -> int
        module N : sig type s val x : s end
    end
end
module type P = sig
    module Priority: Order.Total
end

module M : P = struct module Priority = struct type t = int let compare =
  compare module N = struct type s = int let x = 1 end end end
