type 'a cn = .. constraint 'a = < cast: 'a. 'a nm -> 'a; ..>
and 'a nm = Class : 'a cn -> (< cast: 'a. 'a nm -> 'a; ..> as 'a) nm
exception Bad_cast
class type castable = object method cast: 'a.'a nm -> 'a end
class type foo_t = object inherit castable method foo: string end
type 'a cn += Foo: foo_t cn
class foo: foo_t = object(self)
  method cast: type a. a nm -> a = function Class Foo -> (self :> foo_t)
  method foo = "foo"
end
