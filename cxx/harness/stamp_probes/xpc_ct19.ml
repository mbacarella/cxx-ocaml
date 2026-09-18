module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class type ['a] c = object method a : 'a M.s -> int end
