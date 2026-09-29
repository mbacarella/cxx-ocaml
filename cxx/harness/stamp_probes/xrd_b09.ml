module type Total = sig type t val compare: t -> t -> int end
module type Profile = sig
    module Priority: Total
    class type c = object method code: Priority.t end
end
