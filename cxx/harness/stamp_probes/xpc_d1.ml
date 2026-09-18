module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class type a = object method a : 'a. 'a M.s -> 'a end
